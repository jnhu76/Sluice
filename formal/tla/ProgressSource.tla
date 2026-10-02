---------------- MODULE ProgressSource ----------------
(***************************************************************************)
(* C2-E (Issue #397): the frozen A-D progress/wait protocol of the          *)
(* context-owned ProgressSource, the fixed progress owner's wait loop, and  *)
(* the external-host notification binding, as one focused TLA+ model.      *)
(*                                                                         *)
(* Model follows C++. The implementation (master @ the #444 production      *)
(* corrective pass, the transition-bound close_admission signal) is         *)
(* frozen; this module is its abstract protocol.                           *)
(*                                                                         *)
(* --------------------------------------------------------------------- *)
(* Concrete -> model map (why the model needs it)                          *)
(*                                                                       *)
(*  ProgressSource::mtx_-confined epochs                                  *)
(*    progress_epoch_/progress_exhaustion_   -> pE, pX (saturating pair)   *)
(*    control_epoch_/control_exhaustion_     -> cE, cX (saturating pair)   *)
(*    observed_control_*  (observe_pending_control) -> obsCE, obsCX        *)
(*    acknowledged_control_* (acknowledge_control)   -> ackCE, ackCX       *)
(*    : control pending = spent domain OR current # acknowledged; the      *)
(*      spent pair {MaxCE,MaxCX} is the absorbing never-park state        *)
(*      (control_domain_spent_). Needed for S4 sticky control and V24.    *)
(*                                                                       *)
(*  eventfd notification                  -> fd (BOOLEAN readiness)        *)
(*    : write(1)/EAGAIN saturation and read-drain coalesce to one bit;    *)
(*      producers bump under the mutex first, write fd after release.     *)
(*      Bump and fd write are one atomic step: the only interleaving the  *)
(*      one-mutex-hold revalidation+drain excludes is owner-sees-bump-     *)
(*      and-returns (never parks), which cannot lose a wake.              *)
(*                                                                       *)
(*  Kernel producer (io_uring_register_eventfd CQE publication)           *)
(*    -> KernelComplete: acc-1, cq+1, fd:=TRUE, NO epoch bump             *)
(*    : the fact the userspace epochs cannot express (S2/S8, PROG-02).    *)
(*      No epoch encodes it; on the park path the only cq observer is     *)
(*      the physical probe (passes also reap cq, as the C++ pass does).   *)
(*                                                                       *)
(*  has_immediate_physical_work() probe   -> OProbe reads cq              *)
(*  wait_if_unchanged L4a (one mutex hold:                               *)
(*    health, control, token revalidation, deadline, stale drain)         *)
(*    -> OL4a; the drain sets fd:=FALSE                                   *)
(*  wait_if_unchanged P3 (non-draining revalidation) -> OP3               *)
(*  poll(2) park                          -> pc="park" + OWake/OTimer*    *)
(*  non-EINTR poll failure (sticky health_failed_) -> OPollFail           *)
(*                                                                       *)
(*  AsyncIoContext::wait_one loop                                         *)
(*    token snapshot -> run_progress_pass_ -> completed/health/control/    *)
(*    quiescent-idle report -> park                                       *)
(*    -> OWTop (snapshot+pass+outcome one atomic linearization point;     *)
(*       every constituent C++ step is mutex-atomic itself, and every     *)
(*       intervening producer race has the same observable outcome as one *)
(*       of the two adjacent linearizations)                              *)
(*    dispatch_retry_remains nap (kTransportRetryInterval) -> parkKind    *)
(*      "nap"; caller deadline -> "caller"                                *)
(*  run_progress_pass_ (entry-bounded: dispatch attempt, entry-bounded    *)
(*    reap, publish-all-post-reap snapshot bound, owed sweep; composed    *)
(*    health; close_admission_on_progress_exhaustion_)                    *)
(*    -> the pass effect shared by OWTop/OFinalPass/ODrive. The C++ pass  *)
(*    reaps every CQE visible at its entry snapshot and publishes every   *)
(*    entry present after its own reap; work arriving during the pass is  *)
(*    the next pass's immediate work, so the atomic image is the          *)
(*    deterministic full drain, with concurrent arrivals appearing as     *)
(*    env actions between owner steps.                                   *)
(*  poll_progress()/poll() external drive -> ODrive                       *)
(*  acknowledge_progress_control() -> OAckControl (ack := observed)       *)
(*  interrupt_progress_waiters() -> Interrupt (env)                       *)
(*                                                                       *)
(*  Signal sources (from the C2-B/C audits, retained verbatim at HEAD):   *)
(*    worker physical completion push      -> WorkerComplete (bump+fd)    *)
(*    zero-op delayed reclaim/control pin  -> ZeroOp (owed, bump+fd)      *)
(*    retained transport after accept      -> SubmitTransport (bump+fd)   *)
(*    backend poison (+core note_health_failure closes admission)         *)
(*      -> EnvPoison (physSick, adm:=FALSE, bump+fd)                      *)
(*    application close_admission/request_stop -> CloseAdmission          *)
(*      (the backend signals only on the core-reported open->closed       *)
(*      transition; a repeated close on an already-closed admission has   *)
(*      no observable protocol effect and is a stuttering step here)      *)
(*    successful kernel submit            -> TransportGrant (silent)      *)
(*    plain submit                         -> Submit (silent; the         *)
(*      ThreadPool dispatch ring and the uring dispatch-time submit are   *)
(*      worker/kernel-actionable, not owner-actionable: accepted work     *)
(*      parks normally until its completion signals)                      *)
(*                                                                       *)
(*  Owner/enforcement (S1): driving is one actor. The C++ owner_thread_/  *)
(*  owner_claimed_/drive_active_ admission (invalid_state rejections)     *)
(*  changes no protocol state and is evidenced by the deterministic       *)
(*  ownership/external-loop suites; the model proves the protocol-level   *)
(*  consequence (no interleaved drive/park steps exist).                  *)
(*                                                                       *)
(*  External host binding (S9): progress_notification_fd() marks the      *)
(*    interest live -> HostBorrow; the reversed detach ordering (host     *)
(*    unregisters and retires callbacks FIRST) -> HostUnregister before  *)
(*    DetachHost; acknowledge is a plain drain -> HostAck; teardown      *)
(*    fail-fast on a live binding -> CtxTeardown guard. Detach is not     *)
(*    terminal: a later host may re-lend (HostBorrow's guard re-enables). *)
(*                                                                       *)
(* Deliberately not modelled (out of C2 scope): Scheduler/Fiber,          *)
(* Completion<T> payloads, RequestCore slot/generation machinery (only    *)
(* the owed reclaim obligation survives, as `owed`), the Linux fd table,  *)
(* io_uring SQ/CQ mechanics beyond the cq fact, POLLNVAL fail-fast        *)
(* (lifetime violation = terminate, a contract failure outside the        *)
(* protocol), EINTR poll retries, and clock arithmetic (deadline expiry   *)
(* is a nondeterministic timer action).                                   *)
(*                                                                       *)
(* Mutant switches (must-violate runs certify the checks bite):           *)
(*   MutParkDrain - OL4a drains without revalidating (the C2-B round-1    *)
(*                  sticky-bool defect class) -> InvSafePark must fail   *)
(*   MutNoProbe   - the physical probe ignores CQ state (M-C1 class)      *)
(*                  -> InvSafePark must fail                              *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS MaxPE,          \* progress epoch domain top (uint64 -> small)
          MaxPX,          \* progress exhaustion sequence top
          MaxCE,          \* control epoch domain top
          MaxCX,          \* control exhaustion sequence top
          MaxAcc,         \* accepted work bound (kernel/worker in flight)
          MaxAccT,        \* accepted-but-undispatched transport bound
          MaxCq,          \* unreaped kernel completion bound
          MaxPub,         \* publication-pending bound
          MaxOwed,        \* delayed control/reclaim obligation bound
          MaxFuel,        \* submit fuel: bounds the total workload
          HostOn,         \* include the external-host binding lifecycle
          DeadlineOn,     \* include caller-deadline paths and S6 ghosts
          InitMode,       \* "std" | "satP" | "termP" | "termC":
                          \*   seam-placed initial states. "satP" mirrors
                          \*   set_progress_epoch_for_test(MAX); "termC"
                          \*   mirrors set_control_epoch_for_test(MAX) +
                          \*   set_control_exhaustion_for_test(MAX).
                          \*   "termP" (the absorbing progress pair) is a
                          \*   modeled placement with NO existing C++ seam:
                          \*   set_progress_exhaustion_for_test does not
                          \*   exist and is deliberately not added
          MutParkDrain,   \* mutant: park handshake drains unvalidated
                          \*   (L4a and P3 revalidation both disabled)
          MutNoProbe,     \* mutant: physical probe blind to CQ state
          MutNoExhaustion,\* mutant: saturated epoch bumps nothing (the
                          \*   C2-B round-1 sticky-bool defect class)
          MutRepeatCloseSignals,
                          \* mutant restoring the pre-#444 production defect:
                          \*   backend close_admission() signaled
                          \*   signal_ready_progress() on EVERY invocation,
                          \*   including calls on an already-closed admission
                          \*   (the core close is idempotent; the old signal
                          \*   was not). FALSE in every normal configuration;
                          \*   TRUE only in the MutRepeatCloseSignals*
                          \*   discrimination runs, which must reproduce the
                          \*   reachable absorbing pair and its lost-wake
                          \*   composition from normal Init
          UnboundedWorkload,
                          \* reachability campaign (MaxFuel audit): plain
                          \*   Submit no longer consumes submit fuel.
                          \*   Production has no cumulative submission cap:
                          \*   accepts are bounded by slot capacity plus
                          \*   owner publication service, which the model
                          \*   images through acc/accT/pub/owed caps.
                          \*   SubmitTransport and ZeroOp keep their fuel
                          \*   consumption: their C++ counterparts consume a
                          \*   physically bounded transport-ledger entry /
                          \*   owed control pin per accept. FALSE in every
                          \*   pre-adjudication configuration
          NoExhClose      \* reachability mutant: the pass does not close
                          \*   admission when the progress exhaustion
                          \*   sequence is nonzero -- removes the production
                          \*   bound that keeps request churn finite after
                          \*   exhaustion begins (close_admission_on_
                          \*   progress_exhaustion_). FALSE in every
                          \*   pre-adjudication configuration

VARIABLES pE, pX,         \* progress epoch / exhaustion (saturating pair)
          cE, cX,         \* control epoch / exhaustion (saturating pair)
          obsCE, obsCX,   \* observed control generation
          ackCE, ackCX,   \* acknowledged control generation
          fd,             \* notification readiness (eventfd)
          acc,            \* accepted, physically in flight
          accT,           \* accepted, transport not yet in the kernel
          cq,             \* kernel-published, unreaped completions
          pub,            \* terminal chosen, publication owed
          owed,           \* delayed control/reclaim obligation (L5)
          fuel,           \* remaining submissions
          adm,            \* admission open
          waitSick,       \* sticky wait-domain health verdict
          physSick,       \* backend poison / core health verdict
          pc,             \* owner program point
          tok,            \* park token (epoch pair snapshot)
          parkKind,       \* none | nap (dispatch retry) | caller deadline
          justRet,        \* last owner wait outcome (cleared by env steps)
          hostPh,         \* hNone | hReg | hUnreg
          interest,       \* external notification interest live
          torn,           \* context teardown completed
          gWork,          \* ghost: outstanding sum at wait entry (S6)
          gDone           \* ghost: units published/delivered so far (S6)

vars == <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
          acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
          pc, tok, parkKind, justRet, hostPh, interest, torn,
          gWork, gDone>>

PCs == {"idle", "wtop", "l4a", "probe", "p3", "park", "fp"}
NoTokPair == << -1, -1 >>
RetKinds == {"none", "r0", "rN", "rc", "rd", "rh"}
HostPhs == {"hNone", "hReg", "hUnreg"}

PPair == <<pE, pX>>
CPair == <<cE, cX>>
ObsPair == <<obsCE, obsCX>>
AckPair == <<ackCE, ackCX>>

LexLE(a, b) == a[1] < b[1] \/ (a[1] = b[1] /\ a[2] <= b[2])
LexLT(a, b) == a[1] < b[1] \/ (a[1] = b[1] /\ a[2] < b[2])

(*************************************************************************)
(* Derived protocol predicates (exact images of the C++ definitions).     *)
(*************************************************************************)

ControlSpent == cE = MaxCE /\ cX = MaxCX

ControlPending == ControlSpent \/ cE # ackCE \/ cX # ackCX

ProgressChanged == tok[1] >= 0 /\ (pE # tok[1] \/ pX # tok[2])

ImmWork == pub > 0 \/ cq > 0
AccWork  == acc + accT > 0
RetryOwed == accT > 0
Sick == waitSick \/ physSick

ActionableUserspace == pub > 0 \/ owed > 0 \/ cq > 0 \/ accT > 0

(*************************************************************************)
(* Producer saturation discipline. Each domain saturates instead of      *)
(* wrapping; once fully spent the pair freezes and only the fd write      *)
(* remains. Faithful to signal()/interrupt() at HEAD.                    *)
(*************************************************************************)

BumpP ==
  \/ /\ pE < MaxPE
     /\ pE' = pE + 1 /\ pX' = pX /\ fd' = TRUE
  \/ /\ pE = MaxPE /\ pX < MaxPX /\ ~MutNoExhaustion
     /\ pX' = pX + 1 /\ pE' = pE /\ fd' = TRUE
  \/ /\ pE = MaxPE /\ (pX = MaxPX \/ MutNoExhaustion)
     /\ pE' = pE /\ pX' = pX /\ fd' = TRUE

BumpC ==
  \/ /\ cE < MaxCE
     /\ cE' = cE + 1 /\ cX' = cX /\ fd' = TRUE
  \/ /\ cE = MaxCE /\ cX < MaxCX
     /\ cX' = cX + 1 /\ cE' = cE /\ fd' = TRUE
  \/ /\ cE = MaxCE /\ cX = MaxCX
     /\ cE' = cE /\ cX' = cX /\ fd' = TRUE

(*************************************************************************)
(* Owner actions. The pass effect (textually shared by OWTop, OFinalPass  *)
(* and ODrive) is the deterministic full drain of everything visible at   *)
(* the pass boundary: reaped CQEs become publications, all publications   *)
(* publish, all owed obligations deliver, admission closes when the       *)
(* progress exhaustion sequence is nonzero. doneNow is the pass's         *)
(* completed count.                                                       *)
(*************************************************************************)

OWTop ==
  /\ pc \in {"idle", "wtop"}
  /\ cq' = 0
  /\ pub' = 0
  /\ owed' = 0
  /\ adm' = IF pX > 0 /\ ~NoExhClose THEN FALSE ELSE adm
  /\ gDone' = IF DeadlineOn THEN gDone + cq + pub + owed ELSE 0
  /\ gWork' = IF ~DeadlineOn THEN 0
              ELSE IF pc = "idle" THEN acc + accT + cq + pub + owed
              ELSE gWork
  /\ IF cq + pub + owed > 0
       THEN /\ justRet' = "rN"
            /\ obsCE' = obsCE /\ obsCX' = obsCX
            /\ pc' = "idle" /\ tok' = NoTokPair /\ parkKind' = "none"
     ELSE IF Sick
       THEN /\ justRet' = "rh"
            /\ obsCE' = obsCE /\ obsCX' = obsCX
            /\ pc' = "idle" /\ tok' = NoTokPair /\ parkKind' = "none"
     ELSE IF ControlPending
       THEN /\ obsCE' = cE /\ obsCX' = cX
            /\ justRet' = "rc"
            /\ pc' = "idle" /\ tok' = NoTokPair /\ parkKind' = "none"
     ELSE IF ~ImmWork /\ ~AccWork /\ ~RetryOwed
       THEN /\ justRet' = "r0"
            /\ obsCE' = obsCE /\ obsCX' = obsCX
            /\ pc' = "idle" /\ tok' = NoTokPair /\ parkKind' = "none"
     ELSE /\ \E b \in BOOLEAN :
              /\ tok' = PPair
              /\ obsCE' = obsCE /\ obsCX' = obsCX
              /\ parkKind' = IF accT > 0
                               THEN "nap"
                               ELSE IF b /\ DeadlineOn
                                 THEN "caller"
                                 ELSE "none"
              /\ pc' = "l4a"
              /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, ackCE, ackCX, fd, acc, accT,
                 fuel, waitSick, physSick, hostPh, interest, torn>>

OL4a ==
  /\ pc = "l4a"
  /\ \/ /\ waitSick
        /\ justRet' = "rh"
        /\ pc' = "idle" /\ tok' = NoTokPair /\ parkKind' = "none" /\ fd' = fd
     \/ /\ ~waitSick
        /\ \/ /\ ~MutParkDrain /\ (ControlPending \/ ProgressChanged)
              /\ pc' = "wtop" /\ fd' = fd
              /\ justRet' = "none"
              /\ tok' = tok /\ parkKind' = parkKind
           \/ /\ ~(~MutParkDrain /\ (ControlPending \/ ProgressChanged))
              /\ \E exp \in BOOLEAN :
                   \/ /\ exp /\ DeadlineOn
                      /\ pc' = "fp" /\ fd' = fd
                      /\ justRet' = "none"
                      /\ tok' = tok /\ parkKind' = parkKind
                   \/ /\ ~(exp /\ DeadlineOn)
                      /\ fd' = FALSE
                      /\ pc' = "probe"
                      /\ justRet' = "none"
                      /\ tok' = tok /\ parkKind' = parkKind
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 hostPh, interest, torn, gWork, gDone>>

OProbe ==
  /\ pc = "probe"
  /\ IF ~MutNoProbe /\ cq > 0
       THEN pc' = "wtop"
       ELSE pc' = "p3"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 tok, parkKind, justRet, hostPh, interest, torn,
                 gWork, gDone>>

OP3 ==
  /\ pc = "p3"
  /\ IF ~MutParkDrain /\ (ControlPending \/ ProgressChanged)
       THEN pc' = "wtop"
       ELSE pc' = "park"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 tok, parkKind, justRet, hostPh, interest, torn,
                 gWork, gDone>>

OWake ==
  /\ pc = "park"
  /\ fd
  /\ pc' = "wtop"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 tok, parkKind, justRet, hostPh, interest, torn,
                 gWork, gDone>>

(* Internal dispatch-retry nap expiry: only schedules another bounded
   pass; never a caller outcome. *)
OTimerNap ==
  /\ pc = "park"
  /\ parkKind = "nap"
  /\ pc' = "wtop"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 tok, parkKind, justRet, hostPh, interest, torn,
                 gWork, gDone>>

(* The caller's own wait bound expired. The final bounded pass runs first;
   the deadline never cancels or settles accepted work (S6). *)
OTimerCaller ==
  /\ pc = "park"
  /\ DeadlineOn
  /\ parkKind \in {"nap", "caller"}
  /\ pc' = "fp"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 tok, parkKind, justRet, hostPh, interest, torn,
                 gWork, gDone>>

OFinalPass ==
  /\ pc = "fp"
  /\ cq' = 0
  /\ pub' = 0
  /\ owed' = 0
  /\ adm' = IF pX > 0 /\ ~NoExhClose THEN FALSE ELSE adm
  /\ gDone' = IF DeadlineOn THEN gDone + cq + pub + owed ELSE 0
  /\ justRet' = IF cq + pub + owed > 0 THEN "rN"
                ELSE IF Sick THEN "rh"
                ELSE "rd"
  /\ pc' = "idle" /\ tok' = NoTokPair /\ parkKind' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd, acc, accT,
                 fuel, waitSick, physSick, hostPh, interest, torn, gWork>>

(* A non-EINTR poll(2) failure inside the park: the notification domain can
   no longer distinguish silence from a lost wake, so the verdict is sticky
   and every later wait observes health_failure. *)
OPollFail ==
  /\ pc = "park"
  /\ waitSick' = TRUE
  /\ justRet' = "rh"
  /\ pc' = "idle" /\ tok' = NoTokPair /\ parkKind' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, physSick,
                 hostPh, interest, torn, gWork, gDone>>

(* External host / application drive: poll_progress() bounded pass without
   a wait outcome. *)
ODrive ==
  /\ pc = "idle"
  /\ cq' = 0
  /\ pub' = 0
  /\ owed' = 0
  /\ adm' = IF pX > 0 /\ ~NoExhClose THEN FALSE ELSE adm
  /\ gDone' = IF DeadlineOn THEN gDone + cq + pub + owed ELSE 0
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd, acc, accT,
                 fuel, waitSick, physSick, pc, tok, parkKind,
                 hostPh, interest, torn, gWork>>

(* Owner acknowledgement retires exactly the observed control generation,
   never a later arrival (S4). *)
OAckControl ==
  /\ pc = "idle"
  /\ ackCE' = obsCE
  /\ ackCX' = obsCX
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn,
                 gWork, gDone>>

(*************************************************************************)
(* Environment / producer actions. Other producers may act concurrently  *)
(* with the owner (THREAD-01); every transition that creates actionable  *)
(* work leaves fd readiness or an epoch move the owner's handshake can   *)
(* observe.                                                              *)
(*************************************************************************)

Submit ==
  /\ (UnboundedWorkload \/ fuel > 0)
  /\ adm
  /\ acc + accT < MaxAcc
  /\ acc' = acc + 1
  /\ fuel' = IF UnboundedWorkload THEN fuel ELSE fuel - 1
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 accT, cq, pub, owed, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

(* Retryable transport failure at accept: accepted work outside the kernel
   plus the retained-transport signal (V12 / U2b shape). The slot-capacity
   guard matches Submit and the C++ acceptance transaction: a retained
   transport occupies a slot until its kernel grant, so accept at full
   occupancy is refused (BOUND-01/02). Unobservable in every
   pre-adjudication configuration, where the fuel budget already excludes
   the states it removes. *)
SubmitTransport ==
  /\ fuel > 0
  /\ adm
  /\ acc + accT < MaxAcc
  /\ accT < MaxAccT
  /\ accT' = accT + 1
  /\ fuel' = fuel - 1
  /\ BumpP
  /\ justRet' = "none"
  /\ UNCHANGED <<cE, cX, obsCE, obsCX, ackCE, ackCX,
                 acc, cq, pub, owed, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

(* Worker physical completion: terminal chosen, publication owed, signal
   (the ThreadPool push and the uring zero-op / cancel-won push shape). *)
WorkerComplete ==
  /\ acc > 0
  /\ pub < MaxPub
  /\ acc' = acc - 1
  /\ pub' = pub + 1
  /\ BumpP
  /\ justRet' = "none"
  /\ UNCHANGED <<cE, cX, obsCE, obsCX, ackCE, ackCX,
                 accT, cq, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

(* Kernel completion publication (io_uring CQE): fd readiness only; the
   userspace epochs never move (S2/S8's kernel-producer fact). *)
KernelComplete ==
  /\ acc > 0
  /\ cq < MaxCq
  /\ acc' = acc - 1
  /\ cq' = cq + 1
  /\ fd' = TRUE
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX,
                 accT, pub, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

(* A later pass's submit attempt finally enters the kernel: silent. *)
TransportGrant ==
  /\ accT > 0
  /\ accT' = accT - 1
  /\ acc' = acc + 1
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 cq, pub, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

(* Zero-op immediate publication: the delayed control/reclaim obligation
   plus its signal (the #394 L5 progress half, V26 shape). *)
ZeroOp ==
  /\ fuel > 0
  /\ adm
  /\ owed < MaxOwed
  /\ owed' = owed + 1
  /\ fuel' = fuel - 1
  /\ BumpP
  /\ justRet' = "none"
  /\ UNCHANGED <<cE, cX, obsCE, obsCX, ackCE, ackCX,
                 acc, accT, cq, pub, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

Interrupt ==
  /\ BumpC
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, obsCE, obsCX, ackCE, ackCX,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

(* Backend poison: sticky backend + core health (note_health_failure
   closes admission) and the recovery walk's signal. *)
EnvPoison ==
  /\ ~physSick
  /\ physSick' = TRUE
  /\ adm' = FALSE
  /\ BumpP
  /\ justRet' = "none"
  /\ UNCHANGED <<cE, cX, obsCE, obsCX, ackCE, ackCX,
                 acc, accT, cq, pub, owed, fuel, waitSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

(* Application close_admission(): the backend signals only on the
   core-reported open->closed transition (the #444 shape). A repeated
   close on an already-closed admission returns false and signals
   nothing -- no observable protocol change, i.e. a stuttering step,
   which [][Next]_vars admits without a dedicated action. *)
CloseAdmission ==
  /\ adm
  /\ adm' = FALSE
  /\ BumpP
  /\ justRet' = "none"
  /\ UNCHANGED <<cE, cX, obsCE, obsCX, ackCE, ackCX,
                 acc, accT, cq, pub, owed, fuel, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

(* Load-bearing mutant: restores exactly the pre-#444 production defect
   -- a close_admission() call on an already-closed admission still
   bumped the progress epoch (unconditional backend signal), creating a
   post-exhaustion progress producer that consumes no resource. Enabled
   only by the MutRepeatCloseSignals discrimination runs, which must
   reproduce the reachable absorbing pair and its no-lost-wake
   composition from normal Init; no other semantics is altered. *)
CloseAdmissionRepeat ==
  /\ MutRepeatCloseSignals
  /\ ~adm
  /\ BumpP
  /\ justRet' = "none"
  /\ UNCHANGED <<cE, cX, obsCE, obsCX, ackCE, ackCX,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

(*************************************************************************)
(* External-host binding lifecycle (S9, W-03/PROG-03, V23 context side).  *)
(* The host must unregister and retire its callbacks BEFORE the context   *)
(* detaches the interest; acknowledgement is a plain drain with no        *)
(* authority; a retired interest may be followed by a new borrow;         *)
(* teardown fails fast on a live binding.                                 *)
(*************************************************************************)

HostBorrow ==
  /\ HostOn
  /\ hostPh = "hNone"
  /\ hostPh' = "hReg"
  /\ interest' = TRUE
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, torn, gWork, gDone>>

HostUnregister ==
  /\ HostOn
  /\ hostPh = "hReg"
  /\ hostPh' = "hUnreg"
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, interest, torn, gWork, gDone>>

DetachHost ==
  /\ HostOn
  /\ hostPh = "hUnreg"
  /\ hostPh' = "hNone"
  /\ interest' = FALSE
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, torn, gWork, gDone>>

(* HostAck models the host servicing its loop while it holds the drive
   (the W-03 host drives through poll_progress, never wait_one), so the
   pc="idle" guard excludes host drains racing the owner's park handshake.
   Those interleavings are safe below the absorbing progress pair: every
   userspace fd write is backed by an epoch move the handshake revalidates,
   kernel writes are covered by the probe; at the absorbing pair the
   recorded C2-E boundary finding applies. *)
HostAck ==
  /\ HostOn
  /\ hostPh = "hReg"
  /\ pc = "idle"
  /\ fd
  /\ fd' = FALSE
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, torn, gWork, gDone>>

CtxTeardown ==
  /\ HostOn
  /\ pc = "idle"
  /\ ~interest
  /\ hostPh = "hNone"
  /\ torn' = TRUE
  /\ justRet' = "none"
  /\ UNCHANGED <<pE, pX, cE, cX, obsCE, obsCX, ackCE, ackCX, fd,
                 acc, accT, cq, pub, owed, fuel, adm, waitSick, physSick,
                 pc, tok, parkKind, hostPh, interest, gWork, gDone>>

(*************************************************************************)
(* Spec.                                                                  *)
(*************************************************************************)

OwnerStep ==
  OWTop \/ OL4a \/ OProbe \/ OP3 \/ OWake \/ OTimerNap \/ OTimerCaller
  \/ OFinalPass \/ OPollFail \/ ODrive \/ OAckControl

EnvStep ==
  Submit \/ SubmitTransport \/ WorkerComplete \/ KernelComplete
  \/ TransportGrant \/ ZeroOp \/ Interrupt \/ EnvPoison \/ CloseAdmission
  \/ CloseAdmissionRepeat

HostStep == HostBorrow \/ HostUnregister \/ DetachHost \/ HostAck \/ CtxTeardown

Next == ~torn /\ (OwnerStep \/ EnvStep \/ HostStep)

Init ==
  /\ pE = IF InitMode \in {"satP", "termP"} THEN MaxPE ELSE 0
  /\ pX = IF InitMode = "termP" THEN MaxPX ELSE 0
  /\ cE = IF InitMode = "termC" THEN MaxCE ELSE 0
  /\ cX = IF InitMode = "termC" THEN MaxCX ELSE 0
  /\ obsCE = 0 /\ obsCX = 0
  /\ ackCE = 0 /\ ackCX = 0
  /\ fd = FALSE
  /\ acc = 0 /\ accT = 0 /\ cq = 0 /\ pub = 0 /\ owed = 0
  /\ fuel = MaxFuel
  /\ adm = TRUE
  /\ waitSick = FALSE /\ physSick = FALSE
  /\ pc = "idle" /\ tok = NoTokPair /\ parkKind = "none" /\ justRet = "none"
  /\ hostPh = "hNone" /\ interest = FALSE /\ torn = FALSE
  /\ gWork = 0 /\ gDone = 0

Spec == Init /\ [][Next]_vars

(*************************************************************************)
(* Fairness (campaign C). Explicit, conditional, per the declared          *)
(* assumptions: owner scheduling at every protocol point, environment     *)
(* completion of accepted work, and transport dispatch permission. No     *)
(* unconditional physical I/O termination is claimed.                     *)
(*************************************************************************)

SpecFair ==
  Spec
  /\ WF_vars(OWTop)
  /\ WF_vars(OL4a)
  /\ WF_vars(OProbe)
  /\ WF_vars(OP3)
  /\ WF_vars(OWake)
  /\ WF_vars(OTimerNap)
  /\ WF_vars(OTimerCaller)
  /\ WF_vars(OAckControl)
  /\ WF_vars(WorkerComplete \/ KernelComplete)
  /\ WF_vars(TransportGrant)

(*************************************************************************)
(* Safety invariants.                                                     *)
(*************************************************************************)

TypeOK ==
  /\ pE \in 0..MaxPE /\ pX \in 0..MaxPX
  /\ cE \in 0..MaxCE /\ cX \in 0..MaxCX
  /\ obsCE \in 0..MaxCE /\ obsCX \in 0..MaxCX
  /\ ackCE \in 0..MaxCE /\ ackCX \in 0..MaxCX
  /\ fd \in BOOLEAN
  /\ acc \in 0..MaxAcc /\ accT \in 0..MaxAccT
  /\ cq \in 0..MaxCq /\ pub \in 0..MaxPub /\ owed \in 0..MaxOwed
  /\ fuel \in 0..MaxFuel
  /\ adm \in BOOLEAN /\ waitSick \in BOOLEAN /\ physSick \in BOOLEAN
  /\ pc \in PCs
  /\ (tok = NoTokPair \/ tok \in ((0..MaxPE) \X (0..MaxPX)))
  /\ parkKind \in {"none", "nap", "caller"}
  /\ justRet \in RetKinds
  /\ hostPh \in HostPhs
  /\ interest \in BOOLEAN /\ torn \in BOOLEAN
  /\ gWork \in Nat /\ gDone \in Nat
  /\ pc = "idle" => tok = NoTokPair
  /\ pc \in {"l4a", "probe", "p3", "park"} => tok[1] >= 0

(* S8/S2: a parked owner with no pending readiness may not be sleeping
   past any actionable obligation. The clauses enumerate exactly the
   park-admission facts of the concrete handshake: userspace-drivable
   work, kernel CQ state, control, health, the dispatch-retry nap, and
   the ordinary accepted-work park. *)
InvSafePark ==
  (pc = "park" /\ ~fd) =>
    ( /\ pub = 0 /\ owed = 0 /\ cq = 0
      /\ (accT = 0 \/ parkKind = "nap")
      /\ ~ControlPending
      /\ ~waitSick /\ ~physSick
      /\ (parkKind = "none" => acc > 0) )

(* S4: control generations never regress and acknowledgement never runs
   past the observed generation. *)
InvControlGenOrder ==
  /\ LexLE(ObsPair, CPair)
  /\ LexLE(AckPair, ObsPair)

(* S4: the spent control domain is permanently pending: no park may be
   licensed while the control pair is spent (the L4a/P3 admission checks
   return interrupted first). An owner already parked when the domain
   becomes spent is woken by the interrupt's fd write, so a spent-domain
   park always carries pending readiness. *)
InvControlSpentNeverParks ==
  ControlSpent => (pc # "park" \/ fd)

(* S5: a health failure may never become the quiescent zero-completion
   report; idle is reachable only from genuine quiescence. *)
InvNoFalseIdle ==
  justRet = "r0" =>
    ( /\ acc = 0 /\ accT = 0 /\ cq = 0 /\ pub = 0 /\ owed = 0
      /\ ~ControlPending
      /\ ~Sick )

(* S6: deadline expiry never cancels or settles accepted work: every
   accepted unit is still accounted for, live or already published. *)
InvDeadlineNoCancel ==
  justRet = "rd" =>
    (acc + accT + cq + pub + owed + gDone >= gWork)

(* Campaign A/C: the progress pair never reaches its absorbing terminal
   state from valid production initialization. Post-#444 structure: the
   only pre-fix unbounded producer (the per-call close_admission signal)
   is gone -- a repeated close is a stuttering step. While pX = 0 the
   request churn may be unbounded in count, but every bump lands in pE,
   which saturates at MaxPE without reaching the pair; pX leaves 0 only
   past that saturation, and the first pX bump licenses the owner's
   next pass to close admission (the exhaustion-close rule). The
   signals that can land after that first pX bump are bounded: the
   still-open window admits at most the free-slot accepts and terminals
   of the in-flight population (<= 2C, slot reuse needs owner service),
   the close transition itself contributes one, and after admission
   closes the outstanding requests plus the one-shot poison contribute
   <= C + 1 -- so pX <= 3C + 2 while pE <= MaxPE. ReachNoFuel (C = 1,
   MaxPX = 8 > 5) carries the fuel-independence claim with fuelless
   plain Submit; the tiny original configurations additionally lean on
   MaxFuel = 1, which is why that claim is NOT made from them.
   Production: the C in this accounting is C_eff -- the maximum number
   of simultaneously live, distinct request identities able to produce
   a counted signal -- not the raw configured request_capacity. Every
   counted signal belongs to one live identity (an accept or a
   terminal; slot reuse needs owner service) or to the close
   transition and the one-shot poison, which are counted separately.
   Each live obligation holds one SlotIndex (uint32; request_key.hpp)
   and simultaneously-live obligations hold pairwise-distinct values:
   the free-slot pool is filled as a bijection over [0, capacity) and
   a value returns to the pool only when its obligation retires, so at
   most one live obligation exists per value. BOUND-01 makes
   identity-domain overflow a setup error, so over valid
   configurations C_eff <= |SlotIndex| = 2^32 by representation
   alone: 3*C_eff + 2 <= 3*2^32 + 2 < 2^34 << UINT64_MAX. The pair is
   therefore unreachable in any execution over valid configurations,
   independent of owner latency. Enforcing that setup rejection on
   every admission path is a separate debt recorded in the conformance
   ledger; this is an identity-domain bound, not a claim that
   arbitrary size_t configuration values are valid today. *)
InvNoProgressTerminal == ~(pE = MaxPE /\ pX = MaxPX)

(* Reachability campaign: the production-staged composition -- an ordinary
   (unbounded) park with a completion's publication pending, no pending
   fd readiness, and a token that can no longer change. The benign race
   (completion arrives while parked, fd = TRUE) is excluded: that owner is
   woken. The defect shape is the drained one: at the absorbing pair the
   completion's signal bumped nothing, the L4a drain consumed its fd
   write, and the handshake licensed an ordinary park past the
   unadvertised obligation (PROG-02 clause 4). *)
InvNoOrdinaryParkPastPublication ==
  ~(pc = "park" /\ parkKind = "none" /\ pub > 0 /\ ~fd /\ ~ProgressChanged)

(* S9: teardown only from a fully retired binding; interest exists only
   while a host registration is live. *)
InvTornClean == torn => (~interest /\ hostPh = "hNone")
InvInterestMatchesHost == hostPh = "hNone" => ~interest

(*************************************************************************)
(* Scenario certificates: the NEGATION of each scenario is configured as  *)
(* an INVARIANT and must be VIOLATED (the house must-be-reachable         *)
(* pattern); each certificate holds in the initial state, so a violation  *)
(* certifies the scenario is reached by a nontrivial execution.           *)
(*************************************************************************)

(* S4 mandated scenario: control A observed and reported, control B
   arrives, A acknowledged -> B remains pending. *)
CertStickyControl ==
  ~( /\ ackCE = obsCE /\ ackCX = obsCX
     /\ LexLT(ObsPair, CPair)
     /\ ControlPending )

(* V24 first-stage saturation: a signal in the epoch-frozen domain moves
   the exhaustion sequence, so the token cannot alias. *)
CertSatFreshness == ~(pE = MaxPE /\ pX >= 1)

(* The spent control domain still reports control from every wait. *)
CertSpentControlReports == ~(ControlSpent /\ justRet = "rc")

(* S9/F3: the host-unregistered-but-interest-live intermediate state is
   the detach ordering the reversed contract requires. *)
CertHostLifecycle == ~(hostPh = "hUnreg" /\ interest)

(*************************************************************************)
(* Liveness properties (campaign C, under SpecFair). All conditional on   *)
(* the declared fairness; none claims unconditional physical I/O          *)
(* termination.                                                          *)
(*************************************************************************)

(* L5/progress + S7: persistent userspace obligations are eventually
   driven by passes. *)
LObligationDrains ==
  [](pub > 0 \/ owed > 0 => <>(pub = 0 /\ owed = 0))

(* Retry fairness: retained transport eventually reaches the kernel when
   the environment permits dispatch. *)
LTransportDispatches == [](accT > 0 => <>(accT = 0))

(* L1-conditional (environment half): accepted work the environment
   allows to complete eventually completes. The owner-side observation is
   carried by LParkWakes (a parked owner runs on readiness) and
   LObligationDrains (obligations are driven); a host drive pass may
   legitimately consume the completion without any wait outcome, so the
   observation property must not require the wait-result form. *)
LAcceptedCompletes == [](acc > 0 => <>(acc = 0))

(* No lost wake: a parked owner with pending readiness eventually runs. *)
LParkWakes == [](pc = "park" /\ fd => <>(pc # "park"))

(* Sticky control is eventually resolved while otherwise quiescent, in one
   of the three legal ways: a fresh owner observation (rc), the owner's
   acknowledgement catching up with the current generation (the observed
   control was retired and no later arrival stands), or the stronger health
   verdict the PROG-04 precedence (completed, then health, then control)
   substitutes for the control report. A spent control domain re-arms the
   obligation at every quiet instant, so it forces infinitely many rc
   observations under owner fairness. *)
LControlObserved ==
  []( (ControlPending /\ ~ActionableUserspace /\ acc = 0 /\ ~Sick) =>
        <>( justRet = "rc"
            \/ (ackCE = cE /\ ackCX = cX)
            \/ Sick ) )

(*************************************************************************)
(* Action-to-C++ traceability (review aid).                               *)
(*************************************************************************)
(*  OWTop            AsyncIoContext::wait_one loop head: snapshot() +      *)
(*                   run_progress_pass_() + outcome precedence             *)
(*                   (completed > health > control > quiescent-idle >     *)
(*                   park admission with the retry nap / caller bound)    *)
(*  OL4a             ProgressSource::wait_if_unchanged fused mutex hold:  *)
(*                   health check, control revalidation, token            *)
(*                   revalidation, deadline, stale-readiness drain        *)
(*  OProbe           has_immediate_physical_work() between drain and P3   *)
(*  OP3              the non-draining post-probe revalidation            *)
(*  OWake            poll(2) POLLIN return (control-first recheck)        *)
(*  OTimerNap        dispatch-retry nap expiry -> another bounded pass    *)
(*  OTimerCaller     caller deadline expiry -> final pass + deadline      *)
(*  OFinalPass       the deadline branch's final bounded pass             *)
(*  OPollFail        non-EINTR poll(2) failure -> sticky health_failed_   *)
(*  ODrive           AsyncIoContext::poll_progress()/poll() drive pass    *)
(*  OAckControl      AsyncIoContext::acknowledge_progress_control()       *)
(*  Submit           a silent accept (normal dispatch is worker/kernel-   *)
(*                   actionable only)                                     *)
(*  SubmitTransport  retryable accept: retained transport + signal        *)
(*  WorkerComplete   ThreadPool worker push / cancel-won push:           *)
(*                   publication owed + signal (zero-op publication is    *)
(*                   ZeroOp below)                                        *)
(*  KernelComplete   io_uring CQE publication: eventfd write, no epoch    *)
(*  TransportGrant   a pass's kernel submit finally accepted (silent)     *)
(*  ZeroOp           zero-op publication: owed control/reclaim + signal   *)
(*  Interrupt        interrupt_progress_waiters()                         *)
(*  EnvPoison        poison_and_recover_locked: fatal_error_ +            *)
(*                   note_health_failure + signal                         *)
(*  CloseAdmission   the public close_admission() operation (backend/     *)
(*                   context; THREAD-01-legal external call, no in-tree   *)
(*                   production caller today -- an over-approximation,    *)
(*                   sound for unreachability): signals only on the       *)
(*                   core-reported open->closed transition (#444); a      *)
(*                   repeated close is a stuttering step. request_stop    *)
(*                   is one-shot control-domain and maps to Interrupt     *)
(*  CloseAdmissionRepeat                                                             *)
(*                   MUTANT (MutRepeatCloseSignals): restores the         *)
(*                   pre-#444 backend shape -- close_admission() signaled *)
(*                   on every call, so already-closed calls pumped the    *)
(*                   progress epoch with no new core fact. The            *)
(*                   MutRepeatCloseSignals* runs must turn this back      *)
(*                   into the reachable absorbing pair from normal Init   *)
(*  UnboundedWorkload (constant)                                          *)
(*                   MaxFuel audit: plain Submit consumes no fuel;        *)
(*                   models the absence of a cumulative submission cap    *)
(*                   in production (accepts bounded by capacity + owner   *)
(*                   publication service, imaged by acc/accT/pub/owed)    *)
(*  NoExhClose (constant)                                                            *)
(*                   mutant removing close_admission_on_progress_        *)
(*                   exhaustion_ from the pass: request churn would pump  *)
(*                   the exhaustion sequence without bound               *)
(*  HostBorrow       progress_notification_fd() marking interest live     *)
(*  HostUnregister   the host's loop unregistration + callback retirement *)
(*                   (precondition of detach, reversed F3 ordering)       *)
(*  DetachHost       detach_progress_host() retiring the interest         *)
(*  HostAck          acknowledge_progress_notification() (plain drain)    *)
(*  CtxTeardown      ~AsyncIoContext() after the fail-fast preconditions  *)
(***************************************************************************)
=============================================================================
