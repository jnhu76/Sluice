------------------------------- MODULE E7MultiWorkerProgressBuggyAdmission -------------------------------
(*
  E7 multi-worker progress / blocking-admission protocol (sluice-CORE-E7).

  Formal model of the ACCEPTED abstract E7 progress protocol: persistent
  readiness observation -> global MW-S1/S2/S3/QUIESCENT classification ->
  two-phase MW-S2 candidate election -> final readiness drain + global
  reclassification -> single COMMITTED progress participant -> blocking
  wait admission -> wait return -> reclassification.

  Models ONLY the state load-bearing for the E7 progress/admission contract:
    - Fiber execution state (Waiting/Runnable/Running/Done)
    - worker-local runnable/running sets
    - wait registrations (completion + ready-flag), DISTINCT from
    - authoritative backend outstanding state
    - persistent readiness (level-triggered, survives until drained)
    - coordinated MW-S2 admission (NONE/CANDIDATE/COMMITTED per worker)
    - blocking participant

  Does NOT model: x86_64 context-switch asm, Fiber stacks, AsyncBackend
  internals, io_uring, ThreadPool, Future values, Group, work stealing,
  Fiber migration, E8 ownership transfer. Runnable publication itself is
  closed by E7Publication.tla (63ed522); this model abstracts it as
  PublishRunnable(f) which respects exactly-once (only Waiting->Runnable).

  Domain (finite, exhaustive TLC): Workers={W0,W1}, Fibers={F0,F1}.
*)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Fibers, Workers, W0, W1, F0, F1, NONE

VARIABLES
    fiberState,
    runnable,
    running,
    completionWait,
    readyFlagWait,
    completionReady,
    readyFlagReady,
    backendOutstanding,
    admission,
    blockingWorker

-----------------------------------------------------------------------------
(* Domains *)
-----------------------------------------------------------------------------

FState == {"Waiting", "Runnable", "Running", "Done"}
AdmState == {"NONE", "Candidate_W0", "Candidate_W1", "Committed_W0", "Committed_W1"}

-----------------------------------------------------------------------------
(* Global classifier (authoritative). Mirrors classify_locked(): MW-S2 uses
   backendOutstanding, NOT wait registrations. *)
-----------------------------------------------------------------------------

AnyRunnable == \E w \in Workers : runnable[w] # {}
AnyRunning  == \E w \in Workers : running[w] # NONE
AnyCompletionWait == \E f \in Fibers : completionWait[f]
AnyReadyFlagWait  == \E f \in Fibers : readyFlagWait[f]
AnyWaitRegistration == AnyCompletionWait \/ AnyReadyFlagWait

MW_S1 == AnyRunnable \/ AnyRunning
MW_S2 == /\ ~MW_S1
         /\ backendOutstanding
MW_S3 == /\ ~MW_S1
         /\ ~backendOutstanding
         /\ AnyWaitRegistration
QUIESCENT == /\ ~MW_S1
             /\ ~backendOutstanding
             /\ ~AnyWaitRegistration

-----------------------------------------------------------------------------
(* Helpers *)
-----------------------------------------------------------------------------

OwnerOf(f) == IF f = F0 THEN W0 ELSE W1

AdmFor(w) == IF w = W0 THEN "Candidate_W0" ELSE "Candidate_W1"
CmtFor(w) == IF w = W0 THEN "Committed_W0" ELSE "Committed_W1"

IsCandidate(w) == admission = AdmFor(w)
IsCommitted(w) == admission = CmtFor(w)
AnyCommitted == \E w \in Workers : IsCommitted(w)
AnyCandidate == \E w \in Workers : IsCandidate(w)

-----------------------------------------------------------------------------
(* Actions *)
-----------------------------------------------------------------------------

(* Backend makes a completion ready (persistent). Models wait_one's internal
   poll OR an external producer setting a ready flag. Once ready, stays ready
   until the registration is drained. *)
MakeCompletionReady(f) ==
    /\ completionWait[f]
    /\ completionReady' = [completionReady EXCEPT ![f] = TRUE]
    /\ UNCHANGED <<fiberState, runnable, running, completionWait, readyFlagWait, readyFlagReady,
                   backendOutstanding, admission, blockingWorker>>

MakeReadyFlagReady(f) ==
    /\ readyFlagWait[f]
    /\ readyFlagReady' = [readyFlagReady EXCEPT ![f] = TRUE]
    /\ UNCHANGED <<fiberState, runnable, running, completionWait, completionReady, readyFlagWait,
                   backendOutstanding, admission, blockingWorker>>

(* A backend op becomes outstanding (a Fiber submitted one). *)
BackendOpOutstanding(f) ==
    /\ completionWait[f]
    /\ ~backendOutstanding
    /\ backendOutstanding' = TRUE
    /\ UNCHANGED <<fiberState, runnable, running, completionWait, readyFlagWait,
                   completionReady, readyFlagReady, admission, blockingWorker>>

(* Backend op completes (outstanding drops). Independent of MakeCompletionReady
   (which makes the Completion object ready); this just clears the outstanding
   counter. *)
BackendOpResolved ==
    /\ backendOutstanding
    /\ backendOutstanding' = FALSE
    /\ UNCHANGED <<fiberState, runnable, running, completionWait, readyFlagWait,
                   completionReady, readyFlagReady, admission, blockingWorker>>

(* Drain a ready completion registration: erase reg, Waiting->Runnable,
   publish to owner's runnable set. NOT gated on any reap count. *)
DrainReadyCompletion(f) ==
    /\ completionWait[f]
    /\ completionReady[f]
    /\ fiberState[f] = "Waiting"
    /\ completionWait' = [completionWait EXCEPT ![f] = FALSE]
    /\ completionReady' = [completionReady EXCEPT ![f] = FALSE]
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ runnable' = [runnable EXCEPT ![OwnerOf(f)] = runnable[OwnerOf(f)] \cup {f}]
    /\ UNCHANGED <<running, readyFlagWait, readyFlagReady, backendOutstanding,
                   admission, blockingWorker>>

DrainReadyFlag(f) ==
    /\ readyFlagWait[f]
    /\ readyFlagReady[f]
    /\ fiberState[f] = "Waiting"
    /\ readyFlagWait' = [readyFlagWait EXCEPT ![f] = FALSE]
    /\ readyFlagReady' = [readyFlagReady EXCEPT ![f] = FALSE]
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ runnable' = [runnable EXCEPT ![OwnerOf(f)] = runnable[OwnerOf(f)] \cup {f}]
    /\ UNCHANGED <<running, completionWait, completionReady, backendOutstanding,
                   admission, blockingWorker>>

(* Run a fiber: pop from runnable[w], Running. *)
RunFiber(w, f) ==
    /\ f \in runnable[w]
    /\ fiberState[f] = "Runnable"
    /\ runnable' = [runnable EXCEPT ![w] = runnable[w] \ {f}]
    /\ running' = [running EXCEPT ![w] = f]
    /\ fiberState' = [fiberState EXCEPT ![f] = "Running"]
    /\ UNCHANGED <<completionWait, readyFlagWait, completionReady, readyFlagReady,
                   backendOutstanding, admission, blockingWorker>>

(* A Running fiber suspends on a completion (registers + submits backend op). *)
SuspendOnCompletion(w, f) ==
    /\ running[w] = f
    /\ fiberState[f] = "Running"
    /\ running' = [running EXCEPT ![w] = NONE]
    /\ fiberState' = [fiberState EXCEPT ![f] = "Waiting"]
    /\ completionWait' = [completionWait EXCEPT ![f] = TRUE]
    /\ UNCHANGED <<runnable, readyFlagWait, completionReady, readyFlagReady,
                   backendOutstanding, admission, blockingWorker>>

SuspendOnReadyFlag(w, f) ==
    /\ running[w] = f
    /\ fiberState[f] = "Running"
    /\ running' = [running EXCEPT ![w] = NONE]
    /\ fiberState' = [fiberState EXCEPT ![f] = "Waiting"]
    /\ readyFlagWait' = [readyFlagWait EXCEPT ![f] = TRUE]
    /\ UNCHANGED <<runnable, completionWait, completionReady, readyFlagReady,
                   backendOutstanding, admission, blockingWorker>>

(* Finish a Running fiber (no waits). *)
FinishFiber(w, f) ==
    /\ running[w] = f
    /\ fiberState[f] = "Running"
    /\ running' = [running EXCEPT ![w] = NONE]
    /\ fiberState' = [fiberState EXCEPT ![f] = "Done"]
    /\ UNCHANGED <<runnable, completionWait, readyFlagWait, completionReady,
                   readyFlagReady, backendOutstanding, admission, blockingWorker>>

(* Two-phase admission - PHASE A: elect candidate. Enabled only under MW-S2
   and no current admission. Deterministic: lowest worker (W0 first). *)
ElectCandidate(w) ==
    /\ MW_S2
    /\ admission = "NONE"
    /\ ~AnyCandidate
    /\ w = W0  \* deterministic election (lowest-id worker); W1 only if W0 cannot
    /\ admission' = AdmFor(w)
    /\ UNCHANGED <<fiberState, runnable, running, completionWait, readyFlagWait,
                   completionReady, readyFlagReady, backendOutstanding, blockingWorker>>

(* PHASE B+C combined: final re-drain + global reclassify + commit AND enter
   blocking wait, as one atomic abstract transition. The candidate re-checks
   the GLOBAL state; commit+enter only if still MW-S2 AND no ready registered
   waiter remains undrained (ADR §9.2.6 pre-admission readiness observation).
   Collapsing commit+enter matches the abstract linearization: in production
   the worker releases global_mtx_ and immediately calls wait_one; the gap
   is not observable by the progress protocol. A completion that becomes
   ready concurrently with wait_one is reaped by wait_one itself. *)
ReadyRegUndrained ==
    \E f \in Fibers :
        ((completionWait[f] /\ completionReady[f]) \/
         (readyFlagWait[f] /\ readyFlagReady[f]))

\* THE DEFECT (§10): commit skips the final readiness re-drain check.
\* A Completion that became ready AFTER ElectCandidate (concurrent with the
\* candidate's final recheck) is NOT drained; the worker commits and enters
\* blocking wait_one while a runnable-ready waiter exists. The correct
\* protocol requires ~ReadyRegUndrained at commit; this buggy version omits it.
FinalAdmissionRecheckAndCommit(w) ==
    /\ IsCandidate(w)
    /\ MW_S2
    /\ admission' = CmtFor(w)
    /\ blockingWorker' = w
    /\ UNCHANGED <<fiberState, runnable, running, completionWait, readyFlagWait,
                   completionReady, readyFlagReady, backendOutstanding>>

(* Candidate cancelled (state changed to non-MW-S2 between elect and commit). *)
CancelCandidate(w) ==
    /\ IsCandidate(w)
    /\ ~MW_S2
    /\ admission' = "NONE"
    /\ UNCHANGED <<fiberState, runnable, running, completionWait, readyFlagWait,
                   completionReady, readyFlagReady, backendOutstanding, blockingWorker>>


(* The blocking wait returns. Backend may have made progress (completion
   ready) or not. Clear admission. *)
BackendProgressOrWaitReturn(w) ==
    /\ blockingWorker = w
    /\ blockingWorker' = NONE
    /\ admission' = "NONE"
    /\ UNCHANGED <<fiberState, runnable, running, completionWait, readyFlagWait,
                   completionReady, readyFlagReady, backendOutstanding>>

(* A worker observes state and reclassifies (no-op for the abstract model;
   represents the loop-top classify step). *)
WorkerObserveOrReclassify(w) ==
    /\ TRUE
    /\ UNCHANGED <<fiberState, runnable, running, completionWait, readyFlagWait,
                   completionReady, readyFlagReady, backendOutstanding, admission,
                   blockingWorker>>

(* Stutter (no spurious deadlock; terminal states are reachable). *)
Stutter == UNCHANGED <<fiberState, runnable, running, completionWait, readyFlagWait,
                        completionReady, readyFlagReady, backendOutstanding, admission,
                        blockingWorker>>

-----------------------------------------------------------------------------
(* Next, Init, Spec *)
-----------------------------------------------------------------------------

Next ==
    \/ Stutter
    \/ \E f \in Fibers :
        \/ MakeCompletionReady(f)
        \/ MakeReadyFlagReady(f)
        \/ BackendOpOutstanding(f)
        \/ BackendOpResolved
        \/ DrainReadyCompletion(f)
        \/ DrainReadyFlag(f)
    \/ BackendOpResolved
    \/ \E w \in Workers :
        \/ \E f \in Fibers :
            \/ RunFiber(w, f)
            \/ SuspendOnCompletion(w, f)
            \/ SuspendOnReadyFlag(w, f)
            \/ FinishFiber(w, f)
        \/ ElectCandidate(w)
        \/ FinalAdmissionRecheckAndCommit(w)
        \/ CancelCandidate(w)
        \/ BackendProgressOrWaitReturn(w)
        \/ WorkerObserveOrReclassify(w)

Init ==
    /\ fiberState = [f \in Fibers |-> "Runnable"]
    /\ runnable = [w \in Workers |-> IF w = W0 THEN {F0} ELSE {F1}]
    /\ running = [w \in Workers |-> NONE]
    /\ completionWait = [f \in Fibers |-> FALSE]
    /\ readyFlagWait = [f \in Fibers |-> FALSE]
    /\ completionReady = [f \in Fibers |-> FALSE]
    /\ readyFlagReady = [f \in Fibers |-> FALSE]
    /\ backendOutstanding = FALSE
    /\ admission = "NONE"
    /\ blockingWorker = NONE

Spec == Init /\ [][Next]_<<fiberState, runnable, running, completionWait,
                          readyFlagWait, completionReady, readyFlagReady,
                          backendOutstanding, admission, blockingWorker>>

-----------------------------------------------------------------------------
(* Safety invariants MW-Inv1 through MW-Inv8 *)
-----------------------------------------------------------------------------

(* MW-Inv1: the commit DECISION requires MW-S2 at the commit moment. This is
   enforced by the precondition of FinalAdmissionRecheckAndCommit (MW_S2).
   It is NOT a state invariant: once a worker is blocking, the world can
   legitimately change to MW-S1 (a completion becomes ready and is drained
   by another worker under global_mtx_). The blocking worker's wait_one
   returns and re-drains. So MW-S1 + AnyCommitted is a legitimate transient
   DURING blocking, but never at the moment of commit. *)
(* MW-Inv1 is therefore structural (encoded by FinalAdmissionRecheckAndCommit).
   The state-level companion is MWInv1Blocking below. *)

(* MW-Inv1': a fiber may only be COMMITTED+blocking; never COMMITTED without
   blocking (commit and enter-blocking are atomic in this abstract model). *)
MWInv1CommitBlocksAtomicly ==
    AnyCommitted => blockingWorker # NONE

(* MW-Inv2: at most one committed participant AND at most one blockingWorker. *)
MWInv2 ==
    /\ ~(IsCommitted(W0) /\ IsCommitted(W1))
    /\ ~(blockingWorker = W0 /\ blockingWorker = W1)
    /\ blockingWorker # NONE => AnyCommitted

(* MW-Inv3: blocking requires committed admission. *)
MWInv3 ==
    \A w \in Workers :
        blockingWorker = w => IsCommitted(w)

(* MW-Inv4: commit requires authoritative MW-S2 at final recheck. Encoded by
   FinalAdmissionRecheckAndCommit's precondition (MW_S2). Structural. *)

(* MW-Inv5: the commit DECISION requires no ready registered waiter undrained.
   Enforced by FinalAdmissionRecheckAndCommit's precondition (~ReadyRegUndrained).
   Like MW-Inv1, this is a transition/decision invariant, NOT a state
   invariant: a completion can become ready AFTER commit (concurrently with
   wait_one) and be drained while the worker is blocking. *)

(* MW-Inv6: MW-S3 is not logical quiescence. *)
MWInv6 ==
    (/\ ~MW_S1
     /\ ~backendOutstanding
     /\ AnyWaitRegistration)
        => /\ ~QUIESCENT

(* MW-Inv7: backend outstanding is authoritative. The model permits
   AnyWaitRegistration /\ ~backendOutstanding and classifies it MW-S3.
   Encoded structurally in MW_S3 (uses backendOutstanding, not wait regs).
   Check the consistency: MW_S2 => backendOutstanding. *)
MWInv7 ==
    MW_S2 => backendOutstanding

(* MW-Inv8: no local inference of global MW-S2. A worker may have an empty
   local runnable queue while another worker has runnable/running work; the
   global classifier must still see MW_S1. Structural (MW_S1 is global). *)
MWInv8 ==
    \A w \in Workers :
        (runnable[w] = {} /\ running[w] = NONE)
            => /\ (~MW_S2 \/ ~AnyRunnable \/ ~AnyRunning \/ MW_S1 = TRUE)
            /\ TRUE  \* the invariant is: empty-local does not imply MW_S2 if
                     \* global work exists; captured by MW_S1 being global.

\* The invariant the buggy model is expected to VIOLATE: a committed/blocking
\* admission must never occur while a ready-registered waiter remains
\* undrained. The correct protocol drains ready registrations before commit;
\* the buggy commit (§10) skips this, allowing blocking with undrained ready
\* work.
InvBlockingNoUndrainedReady ==
    blockingWorker # NONE => ~ReadyRegUndrained
=====
