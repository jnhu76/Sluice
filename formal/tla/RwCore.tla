---------------- MODULE RwCore ----------------
(***************************************************************************)
(* Stage 5V2.3 (FCB1-POST-V23-STACK-379-385): the AsyncRwLock              *)
(* primitive's transition system, mapping                                  *)
(* `formal/Sluice/Formal/RwLockV2.lean` (`rwPrim`, `PrimStep2`             *)
(* instantiated for `RwSig`) one action per constructor, under the         *)
(* post-#378 Stage-0-V2.3 execution-domain calculus.                       *)
(*                                                                         *)
(* Call-domain census (from the code, scheduler_rwlock.cpp):               *)
(*   try_read   = external-capable (:135-144; succeeds iff the writer      *)
(*                 bit is clear and the queue is empty)                    *)
(*   read_lock  = fiber-bound (:258-288; the admit section resolves        *)
(*                 inline when the fresh node lands at the head of an      *)
(*                 empty queue with the writer bit clear, :165-172)        *)
(*   try_write  = fiber-bound (:290-299; a recursive attempt is a plain    *)
(*                 false, :305-306)                                        *)
(*   write_lock = fiber-bound (:315-344; a recursive attempt is a          *)
(*                 caller-precondition abort, :206-208; the inline grant   *)
(*                 additionally needs zero readers, :223-230)              *)
(*   unlock_read  = external-capable (:346-356; the :349 held-share        *)
(*                 contract; the grant pass fires only at zero)            *)
(*   unlock_write = fiber-bound (:358-366; both the inactive-writer and    *)
(*                 the not-owner cases are caller-precondition aborts,     *)
(*                 :372-377)                                               *)
(*   cancel     = external-capable (:384-400; removes the queued node,     *)
(*                 re-runs the grant pass, publishes the cancelled node;    *)
(*                 the fiber path is out of the modeled call domain -- the *)
(*                 Lean primitive returns none for it, the MutexCore prec.) *)
(*   read/write_lock_until, expire = timed extensions, outside the core    *)
(*                                                                         *)
(* The grant pass both releases share (`rwlock_grant_from_head_locked`,    *)
(* :38-133): a queued writer is served only when the lock is fully free    *)
(* (:60-69); a queued reader drains the maximal leading run of readers,    *)
(* stopping at the first queued writer (:71-111).                          *)
(*                                                                         *)
(* Reachability disclosure: the boots with pre-held shares and queued      *)
(* waiters are the instrumented battery starts (relation-reachable, not    *)
(* primInit-reachable -- acquiring a share the first time is the caller    *)
(* side's mutex traffic).  `Boot` selects the initial state:               *)
(*   "prim"  - the primInit-faithful start (nothing granted, none queued)  *)
(*   "rh1"   - one reader share held, a writer queued behind it            *)
(*   "rq2"   - one reader share held, two readers queued                   *)
(*   "rh2w"  - two reader shares held, a writer queued at the head         *)
(*   "wq1"   - f0 holds the write, a writer queued behind it               *)
(*                                                                         *)
(* Mutant switches (all FALSE in the reference configuration):             *)
(*   MutGrantWrite - M1: the writer claim ignores the reader count         *)
(*                   (killed by InvExclusion)                              *)
(*   MutNoPay      - M2: unlock_read skips the decrement                   *)
(*                   (killed by InvLedger)                                 *)
(*   MutBatchOne   - M3: the reader batch grants only the head             *)
(*                   (under-production: the batch witness is unreachable;  *)
(*                   every safety invariant still holds)                   *)
(*   MutOwnerSkip  - M4: unlock_write's owner gate is dropped at           *)
(*                   admission (killed by InvWriterOwned)                  *)
(***************************************************************************)
EXTENDS Naturals, Sequences
CONSTANT MaxHistory,   \* fuel: bound on recorded observations
          Boot,        \* initial state: see the disclosure above
          MutGrantWrite,
          MutNoPay,
          MutBatchOne,
          MutOwnerSkip

ASSUME Boot \in {"prim", "rh1", "rq2", "rh2w", "wq1"}

Fibers == {"f0", "f1"}
Exts == {"e0"}
Calls == {"rlock", "runlock", "wlock", "wunlock", "rtry", "wtry", "cancel"}
\* The fiber-bound calls: `cancel` is external-capable only (the Lean
\* primitive's fiber path returns none for it, the MutexCore precedent),
\* so a fiber can never submit one.
FibCalls == {"rlock", "runlock", "wlock", "wunlock", "rtry", "wtry"}
Results == {"none", "unit", "t", "f"}
Modes == {"rd", "wr"}

VARIABLES readers,   \* active reader shares (active_readers_)
          writing,   \* the writer bit (writer_active_)
          wowner,    \* the writer's identity slot (writer_owner_)
          waitq,     \* the one waiter FIFO, mode-tagged (waiters_)
          resolved,  \* one-shot outcomes ([f, b]: TRUE granted)
          phase,     \* per-fiber: "fresh" | "waiting" (queued)
          cur,       \* the worker slot; "run" | "ret" | NoCur
          runq,      \* runnable entries [fiber, call, w, fresh]
          exts,      \* in-flight external records
          history,   \* observation trace
          grantR,    \* ghost: reader grants applied (inline+try+batch)
          unlockR,   \* ghost: unlock_read sections run
          batch_ready, \* ghost: readers readied by the last batch
          hit_inline_read,  \* coverage: a fiber read_lock granted inline
          hit_inline_write, \* coverage: a fiber write_lock granted inline
          hit_wclaim,       \* coverage: a release claimed a queued writer
          hit_tryfail,      \* coverage: a try refused by contention
          hit_cancel_hit,   \* coverage: a cancel of a queued waiter ran
          hit_cancel_miss,  \* coverage: a cancel of a non-waiter ran
          hit_resume,       \* coverage: a resumed waiter completed
          wrel_owned, \* ghost: every unlock_write ran from the owner
          fin_backed  \* ghost: every waiter finish consumed its record

vars == <<readers, writing, wowner, waitq, resolved, phase, cur, runq,
          exts, history, grantR, unlockR, batch_ready,
          hit_inline_read, hit_inline_write, hit_wclaim, hit_tryfail,
          hit_cancel_hit, hit_cancel_miss, hit_resume, wrel_owned,
          fin_backed>>

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

Fresh(c) == c.phase = "run" /\ c.resumed = FALSE
Returning(c) == c.phase = "ret"

Init ==
  /\ readers = IF Boot = "rh2w" THEN 2
               ELSE IF Boot = "rh1" \/ Boot = "rq2" THEN 1
               ELSE 0
  /\ writing = (Boot = "wq1")
  /\ wowner = IF Boot = "wq1" THEN "f0" ELSE "none"
  /\ waitq = IF Boot = "rh1" THEN <<[f |-> "f0", m |-> "wr"]>>
             ELSE IF Boot = "rq2" THEN <<[f |-> "f0", m |-> "rd"],
                                          [f |-> "f1", m |-> "rd"]>>
             ELSE IF Boot = "rh2w" THEN <<[f |-> "f0", m |-> "wr"]>>
             ELSE IF Boot = "wq1" THEN <<[f |-> "f1", m |-> "wr"]>>
             ELSE << >>
  /\ resolved = << >>
  /\ phase = IF Boot = "prim" THEN AllFresh
                               ELSE [f \in Fibers |-> "waiting"]
  /\ cur = NoCur
  /\ runq = << >>
  /\ exts = {}
  /\ history = << >>
  /\ grantR = IF Boot = "rh2w" THEN 2
              ELSE IF Boot = "rh1" \/ Boot = "rq2" THEN 1
              ELSE 0
  /\ unlockR = 0
  /\ batch_ready = 0
  /\ hit_inline_read = FALSE
  /\ hit_inline_write = FALSE
  /\ hit_wclaim = FALSE
  /\ hit_tryfail = FALSE
  /\ hit_cancel_hit = FALSE
  /\ hit_cancel_miss = FALSE
  /\ hit_resume = FALSE
  /\ wrel_owned = TRUE
  /\ fin_backed = TRUE

RECURSIVE InRunq(_, _), InWaitq(_, _), RemoveW(_, _), ConsumeRec(_, _),
          BatchLen(_)
InRunq(q, f) ==
  IF q = << >> THEN FALSE
  ELSE \/ Head(q).fiber = f
       \/ InRunq(Tail(q), f)

InWaitq(q, f) ==
  IF q = << >> THEN FALSE
  ELSE \/ Head(q).f = f
       \/ InWaitq(Tail(q), f)

\* removeW: drop the FIRST entry of f (the WaitQueue::unlink shape),
\* yielding the reduced queue and the removed entry's mode.
RemoveW(q, f) ==
  IF q = << >> THEN [found |-> FALSE, q |-> << >>, m |-> "rd"]
  ELSE IF Head(q).f = f THEN [found |-> TRUE, q |-> Tail(q), m |-> Head(q).m]
  ELSE LET tl == RemoveW(Tail(q), f)
       IN [found |-> tl.found,
           q |-> IF tl.found THEN <<Head(q)>> \o tl.q ELSE q,
           m |-> tl.m]

\* consumeRec: consume the FIRST record of f, yielding its outcome and
\* the remaining records; `found` is FALSE when f has no record.
ConsumeRec(f, s) ==
  IF s = << >> THEN [found |-> FALSE, b |-> FALSE, rest |-> << >>]
  ELSE IF Head(s).f = f
    THEN [found |-> TRUE, b |-> Head(s).b, rest |-> Tail(s)]
    ELSE LET tl == ConsumeRec(f, Tail(s))
         IN [found |-> tl.found, b |-> tl.b,
             rest |-> IF tl.found THEN <<Head(s)>> \o tl.rest ELSE s]

\* The maximal leading run of queued readers (the batch extent :75-107).
\* Nested IFs, not a guarded disjunction: TLC evaluates disjuncts
\* eagerly, so `Head` must sit behind a branch, not a `\/` arm.
BatchLen(q) ==
  IF q = << >> THEN 0
  ELSE IF Head(q).m # "rd" THEN 0
       ELSE 1 + BatchLen(Tail(q))

\* TRUE iff q is non-empty and its head entry is a queued writer
\* (conjunction, not disjunction-guarded Head: same TLC-eagerness rule).
WriterHead(q) == q # << >> /\ Head(q).m = "wr"

\* Drop the first k entries.  The empty result is the literal `<< >>`:
\* a function constructor over a computed empty domain (e.g. `2..1`)
\* yields a value that fails `Seq` membership in TLC.
DropN(q, k) ==
  IF k >= Len(q) THEN << >>
  ELSE [i \in 1..(Len(q) - k) |-> q[i + k]]

\* The fibers of the first k queued entries (the batch's phase resets).
RECURSIVE QFibSet(_, _)
QFibSet(q, k) ==
  IF k <= 0 \/ q = << >> THEN {}
  ELSE {Head(q).f} \cup QFibSet(Tail(q), k - 1)

\* rwAdmit (RwLockV2.lean): the held-share contract for unlock_read
\* (:349), the owner gate for unlock_write (:372-377; MutOwnerSkip is
\* M4's dropped gate), the recursive-write refusal for write_lock
\* (:206-208).  The tries, read_lock, and cancel are always admissible.
Admits(c, f) ==
  IF c = "runlock" THEN readers > 0
  ELSE IF c = "wunlock" THEN wowner = f \/ MutOwnerSkip
  ELSE IF c = "wlock" THEN wowner # f
  ELSE TRUE

\* The runq entry published for a granted or cancelled waiter.
PubEntry(f, c) == [fiber |-> f, call |-> c, w |-> "none", fresh |-> FALSE]

\* PrimStep2.submit: silent; a between-calls fiber enters the runnable
\* queue.  `w` is cancel's target ("none" otherwise).
FiberSubmit(f, c, w) ==
  /\ f \in Fibers /\ c \in FibCalls
  /\ f # cur.fiber
  /\ ~InRunq(runq, f)
  /\ ~InWaitq(waitq, f)
  /\ Len(history) < MaxHistory
  /\ runq' = Append(runq, [fiber |-> f, call |-> c, w |-> w,
                           fresh |-> TRUE])
  /\ UNCHANGED <<readers, writing, wowner, waitq, resolved, phase, cur,
                 exts, history, grantR, unlockR, batch_ready,
                 hit_inline_read, hit_inline_write, hit_wclaim,
                 hit_tryfail, hit_cancel_hit, hit_cancel_miss,
                 hit_resume, wrel_owned, fin_backed>>

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
  /\ UNCHANGED <<readers, writing, wowner, waitq, resolved, phase, exts,
                 grantR, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

\* PrimStep2.fiberEffect, read_lock's inline grant (:165-172): the fresh
\* node lands at the head of an empty queue with the writer bit clear.
RLockInline ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "rlock"
  /\ writing = FALSE /\ waitq = << >>
  /\ readers' = readers + 1
  /\ grantR' = grantR + 1
  /\ hit_inline_read' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<writing, wowner, waitq, resolved, phase, runq, exts,
                 history, unlockR, batch_ready, hit_inline_write,
                 hit_wclaim, hit_tryfail, hit_cancel_hit,
                 hit_cancel_miss, hit_resume, wrel_owned, fin_backed>>

\* PrimStep2.runPark, read_lock suspends: the caller queues as a reader
\* (register_wait_locked + commit_suspend_locked).  Silent.
RLockPark ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "rlock"
  /\ \/ writing = TRUE \/ waitq # << >>
  /\ waitq' = Append(waitq, [f |-> cur.fiber, m |-> "rd"])
  /\ phase' = [phase EXCEPT ![cur.fiber] = "waiting"]
  /\ cur' = NoCur
  /\ UNCHANGED <<readers, writing, wowner, resolved, runq, exts, history,
                 grantR, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

\* PrimStep2.fiberEffect, write_lock's inline grant (:223-230): the lock
\* fully free and the queue empty.
WLockInline ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "wlock"
  /\ readers = 0 /\ writing = FALSE /\ waitq = << >>
  /\ writing' = TRUE
  /\ wowner' = cur.fiber
  /\ hit_inline_write' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<readers, waitq, resolved, phase, runq, exts, history,
                 grantR, unlockR, batch_ready, hit_inline_read,
                 hit_wclaim, hit_tryfail, hit_cancel_hit,
                 hit_cancel_miss, hit_resume, wrel_owned, fin_backed>>

\* PrimStep2.runPark, write_lock suspends: the caller queues as a
\* writer.  Silent.
WLockPark ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "wlock"
  /\ \/ readers > 0 \/ writing = TRUE \/ waitq # << >>
  /\ waitq' = Append(waitq, [f |-> cur.fiber, m |-> "wr"])
  /\ phase' = [phase EXCEPT ![cur.fiber] = "waiting"]
  /\ cur' = NoCur
  /\ UNCHANGED <<readers, writing, wowner, resolved, runq, exts, history,
                 grantR, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

\* PrimStep2.fiberEffect, unlock_read over two or more shares (:351):
\* the count falls; the grant pass does not fire (it fires only at zero).
\* MutNoPay (M2) skips the decrement.
UnlockReadDec ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "runlock"
  /\ readers >= 2
  /\ readers' = IF MutNoPay THEN readers ELSE readers - 1
  /\ unlockR' = unlockR + 1
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<writing, wowner, waitq, resolved, phase, runq, exts,
                 history, grantR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

\* PrimStep2.fiberEffect, unlock_read reaching zero with an empty queue
\* (:352-355: grant_from_head on an empty queue).
UnlockReadEmpty ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "runlock"
  /\ readers = 1 /\ waitq = << >>
  /\ readers' = IF MutNoPay THEN readers ELSE 0
  /\ unlockR' = unlockR + 1
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<writing, wowner, waitq, resolved, phase, runq, exts,
                 history, grantR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

\* PrimStep2.fiberEffect, unlock_read reaching zero with a queued writer
\* served (:60-69): the claim takes the whole writer slot.  MutGrantWrite
\* (M1) drops the zero-reader guard.
UnlockReadClaim ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "runlock"
  /\ waitq # << >> /\ Head(waitq).m = "wr" /\ writing = FALSE
  /\ Head(waitq).f # cur.fiber
  /\ \/ /\ readers = 1 /\ readers' = 0 /\ unlockR' = unlockR + 1
     \/ /\ MutGrantWrite /\ readers >= 1
        /\ readers' = readers - 1 /\ unlockR' = unlockR
  /\ writing' = TRUE
  /\ wowner' = Head(waitq).f
  /\ waitq' = Tail(waitq)
  /\ resolved' = Append(resolved, [f |-> Head(waitq).f, b |-> TRUE])
  /\ runq' = Append(runq, PubEntry(Head(waitq).f, "wlock"))
  /\ phase' = [phase EXCEPT ![Head(waitq).f] = "fresh"]
  /\ grantR' = grantR
  /\ hit_wclaim' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<exts, history, batch_ready, hit_inline_read,
                 hit_inline_write, hit_tryfail, hit_cancel_hit,
                 hit_cancel_miss, hit_resume, wrel_owned, fin_backed>>

\* PrimStep2.fiberEffect, unlock_read reaching zero with a reader batch
\* served (:71-111): the maximal leading run of readers drains, stopping
\* at the first queued writer.  MutBatchOne (M3) readies only the head.
UnlockReadBatch ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "runlock"
  /\ readers = 1 /\ waitq # << >> /\ Head(waitq).m = "rd"
  /\ writing = FALSE
  /\ unlockR' = unlockR + 1
  /\ IF MutBatchOne
       THEN /\ readers' = 1
            /\ grantR' = grantR + 1
            /\ waitq' = Tail(waitq)
            /\ resolved' = Append(resolved,
                 [f |-> Head(waitq).f, b |-> TRUE])
            /\ runq' = Append(runq, PubEntry(Head(waitq).f, "rlock"))
            /\ phase' = [phase EXCEPT ![Head(waitq).f] = "fresh"]
            /\ batch_ready' = 1
       ELSE /\ readers' = BatchLen(waitq)
            /\ grantR' = grantR + BatchLen(waitq)
            /\ waitq' = DropN(waitq, BatchLen(waitq))
            /\ resolved' = resolved \o
                 [i \in 1..BatchLen(waitq) |-> [f |-> waitq[i].f,
                                                b |-> TRUE]]
            /\ runq' = runq \o
                 [i \in 1..BatchLen(waitq) |-> PubEntry(waitq[i].f,
                                                        "rlock")]
            /\ batch_ready' = BatchLen(waitq)
            /\ phase' = [g \in Fibers |-> IF g \in QFibSet(waitq,
                                 BatchLen(waitq)) THEN "fresh" ELSE phase[g]]
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<writing, wowner, exts, history, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

\* PrimStep2.fiberEffect, unlock_write (:378-381): clear the writer slot
\* and run the grant pass.  Three cases as at unlock_read.  The ghost
\* wrel_owned records the owner discipline; MutOwnerSkip (M4) lets a
\* non-owner through the admission gate and trips it here.
UnlockWriteClaim ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "wunlock"
  /\ wrel_owned' = (wrel_owned /\ (wowner = cur.fiber))
  /\ writing = TRUE /\ readers = 0
  /\ waitq # << >> /\ Head(waitq).m = "wr"
  /\ wowner' = Head(waitq).f
  /\ readers' = 0
  /\ waitq' = Tail(waitq)
  /\ resolved' = Append(resolved, [f |-> Head(waitq).f, b |-> TRUE])
  /\ runq' = Append(runq, PubEntry(Head(waitq).f, "wlock"))
  /\ phase' = [phase EXCEPT ![Head(waitq).f] = "fresh"]
  /\ hit_wclaim' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<writing, exts, history, grantR, unlockR, batch_ready,
                 hit_inline_read, hit_inline_write, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume, fin_backed>>

\* PrimStep2.fiberEffect, unlock_write handing the lock to a reader
\* batch.
UnlockWriteBatch ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "wunlock"
  /\ wrel_owned' = (wrel_owned /\ (wowner = cur.fiber))
  /\ writing = TRUE /\ readers = 0
  /\ waitq # << >> /\ Head(waitq).m = "rd"
  /\ writing' = FALSE
  /\ wowner' = "none"
  /\ IF MutBatchOne
       THEN /\ readers' = 1
            /\ grantR' = grantR + 1
            /\ waitq' = Tail(waitq)
            /\ resolved' = Append(resolved,
                 [f |-> Head(waitq).f, b |-> TRUE])
            /\ runq' = Append(runq, PubEntry(Head(waitq).f, "rlock"))
            /\ phase' = [phase EXCEPT ![Head(waitq).f] = "fresh"]
            /\ batch_ready' = 1
       ELSE /\ readers' = BatchLen(waitq)
            /\ grantR' = grantR + BatchLen(waitq)
            /\ waitq' = DropN(waitq, BatchLen(waitq))
            /\ resolved' = resolved \o
                 [i \in 1..BatchLen(waitq) |-> [f |-> waitq[i].f,
                                                b |-> TRUE]]
            /\ runq' = runq \o
                 [i \in 1..BatchLen(waitq) |-> PubEntry(waitq[i].f,
                                                        "rlock")]
            /\ batch_ready' = BatchLen(waitq)
            /\ phase' = [g \in Fibers |-> IF g \in QFibSet(waitq,
                                 BatchLen(waitq)) THEN "fresh" ELSE phase[g]]
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<exts, history, unlockR, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume, fin_backed>>

\* PrimStep2.fiberEffect, unlock_write over an empty or blocked queue
\* (empty queue, or readers still hold against a queued writer).
UnlockWriteNone ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "wunlock"
  /\ wrel_owned' = (wrel_owned /\ (wowner = cur.fiber))
  /\ writing = TRUE
  /\ \/ waitq = << >>
     \/ readers > 0
  /\ writing' = FALSE
  /\ wowner' = "none"
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<readers, waitq, resolved, phase, runq, exts, history,
                 grantR, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume, fin_backed>>

\* PrimStep2.fiberEffect, try_read (:139-143).
TryRead ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "rtry"
  /\ \/ /\ writing = FALSE /\ waitq = << >>
        /\ readers' = readers + 1
        /\ grantR' = grantR + 1
        /\ hit_tryfail' = hit_tryfail
        /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "t"]
     \/ /\ (writing = TRUE \/ waitq # << >>)
        /\ readers' = readers
        /\ grantR' = grantR
        /\ hit_tryfail' = TRUE
        /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "f"]
  /\ UNCHANGED <<writing, wowner, waitq, resolved, phase, runq, exts,
                 history, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_cancel_hit,
                 hit_cancel_miss, hit_resume, wrel_owned, fin_backed>>

\* PrimStep2.fiberEffect, try_write (:301-313): a recursive attempt is a
\* plain false; the grant needs a fully free lock and an empty queue.
TryWrite ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "wtry"
  /\ \/ /\ wowner = cur.fiber
        /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "f"]
        /\ UNCHANGED <<readers, writing, wowner, waitq, resolved, phase,
                       runq, grantR, hit_tryfail>>
     \/ /\ wowner # cur.fiber
        /\ \/ /\ readers = 0 /\ writing = FALSE /\ waitq = << >>
              /\ writing' = TRUE
              /\ wowner' = cur.fiber
              /\ readers' = readers
              /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "t"]
              /\ UNCHANGED <<waitq, resolved, phase, runq, grantR,
                             hit_tryfail>>
           \/ /\ readers > 0 \/ writing = TRUE \/ waitq # << >>
              /\ hit_tryfail' = TRUE
              /\ readers' = readers
              /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "f"]
              /\ UNCHANGED <<writing, wowner, waitq, resolved, phase,
                             runq, grantR>>
  /\ UNCHANGED <<exts, history, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_cancel_hit,
                 hit_cancel_miss, hit_resume, wrel_owned, fin_backed>>

\* PrimStep2.fiberDone: the physical return (completion observation).
FiberDone ==
  /\ cur # NoCur /\ Returning(cur)
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, cur.call, cur.result))
  /\ cur' = NoCur
  /\ UNCHANGED <<readers, writing, wowner, waitq, resolved, phase, runq,
                 exts, grantR, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

\* PrimStep2.dispatchResumed: silent dispatch of a published waiter.
FiberResume ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = FALSE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> Head(runq).call,
             w |-> "none", phase |-> "run", resumed |-> TRUE,
             result |-> "none"]
  /\ runq' = Tail(runq)
  /\ UNCHANGED <<readers, writing, wowner, waitq, resolved, phase, exts,
                 history, grantR, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

\* PrimStep2.finishDone: a resumed waiter consumes its record (the grant
\* was applied by the granter's section) and completes; read_lock and
\* write_lock both return void.  MutSpurious is not among this stage's
\* mutants: the record-backed consumption IS the discipline here
\* (fin_backed ghost).
WaiterFinish ==
  /\ cur # NoCur /\ cur.phase = "run" /\ cur.resumed
  /\ cur.call \in {"rlock", "wlock"}
  /\ LET c == ConsumeRec(cur.fiber, resolved)
     IN /\ c.found
        /\ resolved' = c.rest
        /\ fin_backed' = fin_backed
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, cur.call, "unit"))
  /\ phase' = [phase EXCEPT ![cur.fiber] = "fresh"]
  /\ hit_resume' = TRUE
  /\ cur' = NoCur
  /\ UNCHANGED <<readers, writing, wowner, waitq, runq, exts, grantR,
                 unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, wrel_owned>>

\* PrimStep2.extApply: an external caller ENTERS a call.  The issue
\* observation is emitted at entry; entry owns no lock state.  Only
\* unlock_read, try_read, and cancel are external-capable.
ExtIssue(x, c, w) ==
  /\ x \in Exts /\ c \in {"runlock", "rtry", "cancel"}
  /\ c # "cancel" \/ w \in Fibers
  /\ \A e \in exts : e.x # x
  /\ Len(history) < MaxHistory
  /\ exts' = exts \union {[x |-> x, call |-> c, w |-> w,
                          phase |-> "ent", result |-> "none"]}
  /\ history' = Append(history, IssueExt(x, c))
  /\ UNCHANGED <<readers, writing, wowner, waitq, resolved, phase, cur,
                 runq, grantR, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

\* PrimStep2.extEffect, unlock_read: the same release as the fiber
\* action, three cases, keyed on the in-flight record.
ExtUnlockDec ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "runlock"
       /\ readers >= 2
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "unit"]}
       /\ readers' = IF MutNoPay THEN readers ELSE readers - 1
       /\ unlockR' = unlockR + 1
  /\ UNCHANGED <<writing, wowner, waitq, resolved, phase, cur, runq,
                 history, grantR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

ExtUnlockEmpty ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "runlock"
       /\ readers = 1 /\ waitq = << >>
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "unit"]}
       /\ readers' = IF MutNoPay THEN readers ELSE 0
       /\ unlockR' = unlockR + 1
  /\ UNCHANGED <<writing, wowner, waitq, resolved, phase, cur, runq,
                 history, grantR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

ExtUnlockClaim ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "runlock"
       /\ waitq # << >> /\ Head(waitq).m = "wr" /\ writing = FALSE
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "unit"]}
       /\ \/ /\ readers = 1 /\ readers' = 0 /\ unlockR' = unlockR + 1
          \/ /\ MutGrantWrite /\ readers >= 1
             /\ readers' = readers - 1 /\ unlockR' = unlockR
       /\ writing' = TRUE
       /\ wowner' = Head(waitq).f
       /\ waitq' = Tail(waitq)
       /\ resolved' = Append(resolved, [f |-> Head(waitq).f, b |-> TRUE])
       /\ runq' = Append(runq, PubEntry(Head(waitq).f, "wlock"))
       /\ phase' = [phase EXCEPT ![Head(waitq).f] = "fresh"]
       /\ grantR' = grantR
       /\ hit_wclaim' = TRUE
  /\ UNCHANGED <<cur, history, batch_ready, hit_inline_read,
                 hit_inline_write, hit_tryfail, hit_cancel_hit,
                 hit_cancel_miss, hit_resume, wrel_owned, fin_backed>>

ExtUnlockBatch ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "runlock"
       /\ readers = 1 /\ waitq # << >> /\ Head(waitq).m = "rd"
       /\ writing = FALSE
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "unit"]}
       /\ unlockR' = unlockR + 1
       /\ IF MutBatchOne
            THEN /\ readers' = 1
                 /\ grantR' = grantR + 1
                 /\ waitq' = Tail(waitq)
                 /\ resolved' = Append(resolved,
                      [f |-> Head(waitq).f, b |-> TRUE])
                 /\ runq' = Append(runq,
                      PubEntry(Head(waitq).f, "rlock"))
                 /\ phase' = [phase EXCEPT ![Head(waitq).f] = "fresh"]
                 /\ batch_ready' = 1
            ELSE /\ readers' = BatchLen(waitq)
                 /\ grantR' = grantR + BatchLen(waitq)
                 /\ waitq' = DropN(waitq, BatchLen(waitq))
                 /\ resolved' = resolved \o
                      [i \in 1..BatchLen(waitq) |-> [f |-> waitq[i].f,
                                                     b |-> TRUE]]
                 /\ runq' = runq \o
                      [i \in 1..BatchLen(waitq) |->
                         PubEntry(waitq[i].f, "rlock")]
                 /\ batch_ready' = BatchLen(waitq)
                 /\ phase' = [g \in Fibers |-> IF g \in QFibSet(waitq,
                                      BatchLen(waitq)) THEN "fresh"
                                      ELSE phase[g]]
  /\ UNCHANGED <<writing, wowner, cur, history, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

\* PrimStep2.extEffect, try_read.
ExtTryRead ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "rtry"
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff",
                          result |-> IF writing = FALSE /\ waitq = << >>
                                       THEN "t" ELSE "f"]}
       /\ \/ /\ writing = FALSE /\ waitq = << >>
            /\ readers' = readers + 1
            /\ grantR' = grantR + 1
            /\ hit_tryfail' = hit_tryfail
         \/ /\ writing = TRUE \/ waitq # << >>
            /\ readers' = readers
            /\ grantR' = grantR
            /\ hit_tryfail' = TRUE
  /\ UNCHANGED <<writing, wowner, waitq, resolved, phase, cur, runq,
                 history, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_cancel_hit,
                 hit_cancel_miss, hit_resume, wrel_owned, fin_backed>>

\* PrimStep2.extEffect, cancel: the same three-way grant-after-removal
\* as the fiber action.
ExtCancelHitNone ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "cancel"
       /\ InWaitq(waitq, e.w)
       /\ LET rm == RemoveW(waitq, e.w)
          IN /\ rm.found
             /\ \/ rm.q = << >>
                \/ writing = TRUE
                \/ WriterHead(rm.q) /\ readers > 0
             /\ waitq' = rm.q
             /\ resolved' = Append(resolved, [f |-> e.w, b |-> FALSE])
             /\ runq' = Append(runq,
                  PubEntry(e.w, IF rm.m = "rd" THEN "rlock" ELSE "wlock"))
             /\ phase' = [phase EXCEPT ![e.w] = "fresh"]
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "t"]}
       /\ hit_cancel_hit' = TRUE
  /\ UNCHANGED <<readers, writing, wowner, cur, history, grantR,
                 unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_miss, hit_resume, wrel_owned, fin_backed>>

ExtCancelHitClaim ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "cancel"
       /\ InWaitq(waitq, e.w)
       /\ LET rm == RemoveW(waitq, e.w)
          IN /\ rm.found
             /\ rm.q # << >> /\ Head(rm.q).m = "wr" /\ writing = FALSE
             /\ readers = 0
             /\ waitq' = Tail(rm.q)
             /\ writing' = TRUE
             /\ wowner' = Head(rm.q).f
             /\ resolved' = Append(Append(resolved,
                  [f |-> Head(rm.q).f, b |-> TRUE]),
                  [f |-> e.w, b |-> FALSE])
             /\ runq' = Append(Append(runq,
                  PubEntry(Head(rm.q).f, "wlock")),
                  PubEntry(e.w, IF rm.m = "rd" THEN "rlock" ELSE "wlock"))
             /\ phase' = [phase EXCEPT ![Head(rm.q).f] = "fresh",
                                          ![e.w] = "fresh"]
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "t"]}
       /\ hit_cancel_hit' = TRUE
       /\ hit_wclaim' = TRUE
  /\ UNCHANGED <<readers, cur, history, grantR, unlockR, batch_ready,
                 hit_inline_read, hit_inline_write, hit_tryfail,
                 hit_cancel_miss, hit_resume, wrel_owned, fin_backed>>

ExtCancelHitBatch ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "cancel"
       /\ InWaitq(waitq, e.w)
       /\ LET rm == RemoveW(waitq, e.w)
          IN /\ rm.found
             /\ rm.q # << >> /\ Head(rm.q).m = "rd" /\ writing = FALSE
             /\ waitq' = IF MutBatchOne
                           THEN Tail(rm.q)
                           ELSE DropN(rm.q, BatchLen(rm.q))
             /\ readers' = IF MutBatchOne THEN readers + 1
                           ELSE readers + BatchLen(rm.q)
             /\ grantR' = grantR + IF MutBatchOne THEN 1
                                   ELSE BatchLen(rm.q)
             /\ resolved' = IF MutBatchOne
                 THEN Append(Append(resolved,
                        [f |-> Head(rm.q).f, b |-> TRUE]),
                        [f |-> e.w, b |-> FALSE])
                 ELSE Append(resolved \o
                        [i \in 1..BatchLen(rm.q) |-> [f |-> rm.q[i].f,
                                                      b |-> TRUE]],
                        [f |-> e.w, b |-> FALSE])
             /\ runq' = IF MutBatchOne
                 THEN Append(Append(runq,
                        PubEntry(Head(rm.q).f, "rlock")),
                        PubEntry(e.w, IF rm.m = "rd" THEN "rlock"
                                       ELSE "wlock"))
                 ELSE Append(runq \o
                        [i \in 1..BatchLen(rm.q) |->
                           PubEntry(rm.q[i].f, "rlock")],
                        PubEntry(e.w, IF rm.m = "rd" THEN "rlock"
                                       ELSE "wlock"))
             /\ batch_ready' = IF MutBatchOne THEN 1 ELSE BatchLen(rm.q)
             /\ phase' = IF MutBatchOne
                 THEN [phase EXCEPT ![Head(rm.q).f] = "fresh",
                                      ![e.w] = "fresh"]
                 ELSE [g \in Fibers |-> IF g \in QFibSet(rm.q,
                            BatchLen(rm.q)) \/ g = e.w THEN "fresh"
                            ELSE phase[g]]
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "t"]}
       /\ hit_cancel_hit' = TRUE
  /\ UNCHANGED <<writing, wowner, cur, history, unlockR,
                 hit_inline_read, hit_inline_write, hit_wclaim,
                 hit_tryfail, hit_cancel_miss, hit_resume, wrel_owned,
                 fin_backed>>

\* PrimStep2.extEffect, cancel of a non-waiter (silent; no state change).
ExtCancelMiss ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "cancel"
       /\ ~InWaitq(waitq, e.w)
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "f"]}
       /\ hit_cancel_miss' = TRUE
  /\ UNCHANGED <<readers, writing, wowner, waitq, resolved, phase, cur,
                 runq, history, grantR, unlockR, batch_ready,
                 hit_inline_read, hit_inline_write, hit_wclaim,
                 hit_tryfail, hit_cancel_hit, hit_resume, wrel_owned,
                 fin_backed>>

\* PrimStep2.extDone: the physical return, deliberately unordered with
\* respect to the other steps.
ExtDone(x) ==
  /\ \E e \in exts : e.x = x /\ e.phase = "eff"
  /\ Len(history) < MaxHistory
  /\ LET e == CHOOSE e \in exts : e.x = x /\ e.phase = "eff"
     IN /\ exts' = exts \ {e}
        /\ history' = Append(history, CompExt(x, e.call, e.result))
  /\ UNCHANGED <<readers, writing, wowner, waitq, resolved, phase, cur,
                 runq, grantR, unlockR, batch_ready, hit_inline_read,
                 hit_inline_write, hit_wclaim, hit_tryfail,
                 hit_cancel_hit, hit_cancel_miss, hit_resume,
                 wrel_owned, fin_backed>>

Next ==
  \/ \E f \in Fibers, c \in Calls, w \in Fibers \cup {"none"} :
       FiberSubmit(f, c, w)
  \/ FiberDispatch
  \/ RLockInline
  \/ RLockPark
  \/ WLockInline
  \/ WLockPark
  \/ UnlockReadDec
  \/ UnlockReadEmpty
  \/ UnlockReadClaim
  \/ UnlockReadBatch
  \/ UnlockWriteClaim
  \/ UnlockWriteBatch
  \/ UnlockWriteNone
  \/ TryRead
  \/ TryWrite
  \/ FiberDone
  \/ FiberResume
  \/ WaiterFinish
  \/ \E x \in Exts, c \in {"runlock", "rtry", "cancel"},
       w \in Fibers \cup {"none"} : ExtIssue(x, c, w)
  \/ ExtUnlockDec
  \/ ExtUnlockEmpty
  \/ ExtUnlockClaim
  \/ ExtUnlockBatch
  \/ ExtTryRead
  \/ ExtCancelHitNone
  \/ ExtCancelHitClaim
  \/ ExtCancelHitBatch
  \/ ExtCancelMiss
  \/ \E x \in Exts : ExtDone(x)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Type and safety invariants                                              *)
(***************************************************************************)

TypeOK ==
  /\ readers \in 0..4
  /\ writing \in BOOLEAN
  /\ wowner \in Fibers \cup {"none"}
  /\ waitq \in Seq({[f |-> g, m |-> md] : g \in Fibers, md \in Modes})
  /\ resolved \in Seq({[f |-> g, b |-> bo] : g \in Fibers, bo \in BOOLEAN})
  /\ phase \in [Fibers -> {"fresh", "waiting"}]
  /\ cur \in ({NoCur} \union
       {[fiber |-> f, call |-> c, w |-> w, phase |-> p, resumed |-> r,
         result |-> s] :
          f \in Fibers, c \in FibCalls, w \in Fibers \cup {"none"},
          p \in {"run", "ret"}, r \in BOOLEAN, s \in Results})
  /\ runq \in Seq({[fiber |-> f, call |-> c, w |-> w, fresh |-> fr] :
                    f \in Fibers, c \in FibCalls,
                    w \in Fibers \cup {"none"}, fr \in BOOLEAN})
  /\ exts \subseteq {[x |-> x, call |-> c, w |-> w, phase |-> p,
                      result |-> r] :
                      x \in Exts,
                      c \in {"runlock", "rtry", "cancel"},
                      w \in Fibers \cup {"none"},
                      p \in {"ent", "eff"}, r \in Results}
  /\ \A e1 \in exts, e2 \in exts : e1.x = e2.x => e1 = e2
  /\ history \in Seq({[type |-> t, src |-> s, id |-> i, call |-> c,
                       result |-> r] :
                       t \in {"issue", "comp"}, s \in {"fib", "ext"},
                       i \in (Fibers \cup Exts), c \in Calls,
                       r \in Results})
  /\ grantR \in 0..9 /\ unlockR \in 0..9 /\ batch_ready \in 0..2
  /\ hit_inline_read \in BOOLEAN /\ hit_inline_write \in BOOLEAN
  /\ hit_wclaim \in BOOLEAN /\ hit_tryfail \in BOOLEAN
  /\ hit_cancel_hit \in BOOLEAN /\ hit_cancel_miss \in BOOLEAN
  /\ hit_resume \in BOOLEAN
  /\ wrel_owned \in BOOLEAN /\ fin_backed \in BOOLEAN

\* The reader-count ledger: every share in the count was granted (inline,
\* try, or batch) and not yet paid for by an unlock_read section.  All
\* effects are atomic in the section actions, so the equation is exact.
InvLedger == readers = grantR - unlockR

\* Mode exclusion (`rwExclP`): the modes exclude each other, and the
\* writer slot is owned exactly while a writer is active.
InvExclusion ==
  /\ writing => readers = 0 /\ wowner # "none"
  /\ ~writing => wowner = "none"

\* The owner discipline (M4's kill): an unlock_write section ran only
\* from the fiber that owned the active writer (:372-377).
InvWriterOwned == wrel_owned

\* Every waiter finish consumed a published record.
InvFinishBacked == fin_backed

InvQueueNoDup ==
  /\ \A i \in 1..Len(waitq), j \in 1..Len(waitq) :
       i # j => waitq[i].f # waitq[j].f
  /\ \A i \in 1..Len(runq), j \in 1..Len(runq) :
       i # j => runq[i].fiber # runq[j].fiber

\* Queue ownership: a queued fiber is never runnable or dispatched, and
\* sits in phase "waiting"; only published lock calls sit stale in the
\* runnable queue.  (A write holder queueing as a reader is reachable --
\* readers are a count -- so no owner-vs-queue conjunct exists here.)
InvQueueOwnership ==
  /\ \A i \in 1..Len(waitq) :
       /\ ~InRunq(runq, waitq[i].f)
       /\ waitq[i].f # cur.fiber
       /\ phase[waitq[i].f] = "waiting"
  /\ \A i \in 1..Len(runq) : runq[i].fiber # cur.fiber
  /\ \A i \in 1..Len(runq) :
       runq[i].fresh = FALSE => runq[i].call \in {"rlock", "wlock"}

SameCaller(o1, o2) == o1.src = o2.src /\ o1.id = o2.id

\* Completion discipline: every completion matches a prior issue of the
\* same call by the same caller, with no intervening completion of that
\* caller -- EXCEPT the boot waiters' and boot holders' single pre-Init
\* call instance (the instrumented configurations start with shares held
\* and waiters parked past their issues, the documented battery
\* disclosure); such a completion has no in-trace issue of that call up
\* to its own position (the exception is prefix-scoped, so the invariant
\* is monotone).
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

SafetyInvariants == <<TypeOK, InvLedger, InvExclusion, InvWriterOwned,
                      InvFinishBacked, InvQueueNoDup, InvQueueOwnership,
                      InvCompDiscipline>>

(***************************************************************************)
(* Coverage predicates and the witness separation                          *)
(*                                                                         *)
(* All are monotone (history append or set-once flags/ghost counters),     *)
(* so a single TLC run reaching the conjunctive state certifies every      *)
(* component; each coverage cfg checks the negated conjunction and must    *)
(* be VIOLATED.  The mutant separation cfg asserts the witness is          *)
(* UNreachable and must HOLD.                                              *)
(***************************************************************************)

\* Battery A: a fresh read_lock granted inline and completed.
CovInlineRead == hit_inline_read /\ hit_resume

\* Battery B: a writer claimed by a release and completed (the handoff).
CovWriterClaim == hit_wclaim /\ hit_resume

\* Battery C: one release readied two or more queued readers (the
\* batch) and a resumed waiter completed.
CovBatch == batch_ready >= 2 /\ hit_resume

\* Battery D: a cancel of a queued waiter and the cancelled waiter's
\* completion.
CovCancel == hit_cancel_hit /\ hit_resume

\* The empty-surface corners: an inline write grant, a refused try, a
\* cancel miss.
CovInlineWrite == hit_inline_write
CovTryFail == hit_tryfail
CovCancelMiss == hit_cancel_miss

\* The M3 witness separation: under MutBatchOne no release readies two
\* readers.
NotBatchWitness == ~CovBatch

\* A state constraint for the witness cfgs: prune every behavior that
\* leaves the witness's own call mix (one external resolver kind, lock
\* submissions only).  A constraint can only SHRINK the explored space,
\* so a violation found under it is still a witness; the PASS cfgs run
\* unconstrained.
WitConstraint ==
  (\A e \in exts : e.call \in {"runlock", "rtry"}) /\
  (\A i \in 1..Len(runq) : runq[i].call \in {"rlock", "wlock", "rtry", "wtry"})

CancelConstraint ==
  (\A e \in exts : e.call = "cancel") /\
  (\A i \in 1..Len(runq) : runq[i].call \in {"rlock", "wlock", "rtry", "wtry"})

InvCovInlineRead == ~CovInlineRead
InvCovWriterClaim == ~CovWriterClaim
InvCovBatch == ~CovBatch
InvCovCancel == ~CovCancel
InvCovInlineWrite == ~CovInlineWrite
InvCovTryFail == ~CovTryFail
InvCovCancelMiss == ~CovCancelMiss

=========================================================================
====
