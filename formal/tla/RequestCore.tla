---------------- MODULE RequestCore ----------------
(***************************************************************************)
(* B1-1 (#394): the standalone RequestCore request-lifecycle protocol.     *)
(*                                                                         *)
(* Source correspondence: the C++ substrate in                             *)
(*   include/sluice/async/detail/request_core.hpp                          *)
(*   src/async/detail/request_core.cpp                                     *)
(* driven by tests/request_core_test_driver.hpp (FakePhysicalDriver).      *)
(* One model action per protocol transition; the driver's injected events  *)
(* map to the same actions.  The model is deliberately free of ThreadPool  *)
(* worker structure, io_uring SQE/CQE structure, Scheduler, Fiber,         *)
(* Observer and ProgressSource vocabulary.                                 *)
(*                                                                         *)
(* Per-slot protocol facts (orthogonal, not one enum):                     *)
(*   phase   free / reserved / accepted / permanently retired               *)
(*   gen     slot generation; advances on release, retires at GenMax       *)
(*   bind    public binding live (class B)                                 *)
(*   term    none / ok / err / canceled  (canonical terminal, immutable)   *)
(*   exec    borrow-touching execution refs (class E)                      *)
(*   ctl     internal control refs (class C)                               *)
(*   pub     none / inflight (P) / done                                    *)
(*   claimed execution claim granted (dispatch handoff happened)           *)
(*   intent  sticky cancel intent (never a terminal by itself)             *)
(*                                                                         *)
(* Ghost monitoring by full request identity, so slot reuse cannot erase   *)
(* an obligation:                                                           *)
(*   stage[id]        0 never / 1 accepted / 2 published /                 *)
(*                    3 binding released / 4 reclaimed (monotone)          *)
(*   acceptCount[id]  number of acceptance commits for that identity       *)
(*   chosenIds        identities for which a terminal was ever selected    *)
(*   choices[s]       this occupancy's terminal choices, cleared on        *)
(*                    release                                              *)
(*   reclaimLog       the pin facts captured at every reclaim              *)
(*                                                                         *)
(* Terminal admissibility floor frozen here:                               *)
(*   - cancel intent alone never selects a terminal;                       *)
(*   - a zero-effect cancel win requires that no execution claim was       *)
(*     granted;                                                             *)
(*   - a physical outcome is admissible only while accepted and unchosen;  *)
(*   - duplicate and stale-generation candidates change nothing.           *)
(*                                                                         *)
(* Execution responsibility (class E) is a frozen capability chain:        *)
(*   - acceptance installs the first E (the pending-dispatch lease);       *)
(*   - a handoff may acquire a new E only while no terminal is chosen;     *)
(*   - the last E may retire only after a terminal holds the outcome       *)
(*     (accepted-and-unchosen keeps E > 0, InvExecHeldUntilTerminal);      *)
(*   - once a terminal is chosen no new E may be acquired                  *)
(*     (InvNoExecRevival).                                                 *)
(*                                                                         *)
(* Admission health: a health failure is a fact next to the closed bit;    *)
(*   both Reserve and Accept refuse under it, and no acceptance commits    *)
(*   after it (InvNoAcceptAfterHealth).                                    *)
(*                                                                         *)
(* Reclaim requires the slot published, binding released, exec and ctl     *)
(* retired.  Control refs cannot be acquired on an already fully-released  *)
(*   slot: new bookkeeping on settled work is outside the protocol, and    *)
(* keeping the predicate monotone is what makes L5 honest.  The C++        *)
(*   acquire_control carries the same settled-work guard explicitly: the   *)
(*   synchronous zero-gap reclaim closes only the C = 0 window; a live     *)
(*   C pin defers reclaim and re-opens the window, so the guard is part    *)
(*   of the API contract, not an optimization.                             *)
(*                                                                         *)
(* This is the repository's first liveness-carrying model: FairSpec adds   *)
(* weak fairness on the driver/physical/retirement/publication/reclaim     *)
(* services, and the cfgs check PROPERTY L1 (accepted ~> published) and    *)
(* L5 (reclaimable ~> reclaimed) by full request identity.  TLA+ does not  *)
(* prove the C++ memory model; publication happens-before is argued        *)
(* separately in docs/roadmap/b1-1-request-core-evidence.md.               *)
(*                                                                         *)
(* Mutant switches (all FALSE in the reference cfg); each Mut* cfg must    *)
(* be killed by its expected invariant or property:                        *)
(*   MutIgnoreClose        accept ignores admission close                  *)
(*   MutIgnoreHealth       accept ignores the health failure               *)
(*   MutRollbackResidue    rollback enabled from accepted, leaves facts    *)
(*   MutTerminalOverwrite  a second candidate overwrites the winner        *)
(*   MutCancelAsPhysical   cancel wins even after the execution claim      *)
(*   MutPublishWhileE      publication may begin while exec > 0            *)
(*   MutReleaseResolvable  binding release forgets to invalidate binding   *)
(*   MutGenWrap            release does not advance the generation         *)
(*   MutReclaimIgnoresPub  reclaim drops the published requirement         *)
(*   MutReclaimIgnoresCtl  reclaim drops the control-pin requirement       *)
(*   MutRetireLastExec     the last exec retires with no terminal chosen   *)
(*   MutExecAfterTerminal  a new exec is acquired after a chosen terminal  *)
(*   MutStaleEvent         a stale-generation outcome hits a reused slot   *)
(*   MutStrandPostAccept   a recorded intent blocks the physical outcome   *)
(*   MutLazyReclaim        reclaim requires an unrelated submit wake       *)
(*   MutCtlAfterSettled    control may be re-acquired on settled work      *)
(*   MutDoubleDecrement    retirement decrements the same ref twice        *)
(*   MutReadyBeforePayload publication may begin with no terminal chosen;  *)
(*                         subsumed by the E-chain guard since Corrective-1 *)
(*                         (exec = 0 with no terminal is unreachable), so  *)
(*                         its cfg must now complete CLEANLY as the        *)
(*                         separation witness for that subsumption         *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANT MutIgnoreClose,
          MutIgnoreHealth,
          MutRollbackResidue,
          MutTerminalOverwrite,
          MutCancelAsPhysical,
          MutPublishWhileE,
          MutReleaseResolvable,
          MutGenWrap,
          MutReclaimIgnoresPub,
          MutReclaimIgnoresCtl,
          MutRetireLastExec,
          MutExecAfterTerminal,
          MutStaleEvent,
          MutStrandPostAccept,
          MutLazyReclaim,
          MutCtlAfterSettled,
          MutDoubleDecrement,
          MutReadyBeforePayload

Slots == {"s0", "s1"}
GenMax == 2
MaxExec == 2
MaxCtl == 1

Ids == [slot: Slots, gen: 0..GenMax]
Phases == {"free", "reserved", "accepted", "retired"}
Terms == {"none", "ok", "err", "canceled"}
Pubs == {"none", "inflight", "done"}
Stages == 0..4
ChoiceRecords == [gen: 0..GenMax, claimed: BOOLEAN, kind: {"phys", "cancelwin"}]
ReclaimRecords == [id: Ids, exec: 0..MaxExec, ctl: 0..MaxCtl,
                   bind: BOOLEAN, pub: Pubs]

VARIABLES phase, gen, bind, term, exec, ctl, pub, claimed, intent, choices,
          closed, health, wake, acceptsAfterClose, acceptsAfterHealth,
          stage, acceptCount, chosenIds, reclaimLog, execRevived

vars == <<phase, gen, bind, term, exec, ctl, pub, claimed, intent, choices,
          closed, health, wake, acceptsAfterClose, acceptsAfterHealth,
          stage, acceptCount, chosenIds, reclaimLog, execRevived>>

Identity(s) == [slot |-> s, gen |-> gen[s]]

UnoccupiedClean(s) ==
  /\ exec[s] = 0
  /\ ctl[s] = 0
  /\ ~bind[s]
  /\ term[s] = "none"
  /\ pub[s] = "none"
  /\ ~claimed[s]
  /\ ~intent[s]
  /\ choices[s] = << >>

Reclaimable(s) ==
  /\ phase[s] = "accepted"
  /\ pub[s] = "done"
  /\ ~bind[s]
  /\ exec[s] = 0
  /\ ctl[s] = 0

Init ==
  /\ phase = [s \in Slots |-> "free"]
  /\ gen = [s \in Slots |-> 0]
  /\ bind = [s \in Slots |-> FALSE]
  /\ term = [s \in Slots |-> "none"]
  /\ exec = [s \in Slots |-> 0]
  /\ ctl = [s \in Slots |-> 0]
  /\ pub = [s \in Slots |-> "none"]
  /\ claimed = [s \in Slots |-> FALSE]
  /\ intent = [s \in Slots |-> FALSE]
  /\ choices = [s \in Slots |-> << >>]
  /\ closed = FALSE
  /\ health = FALSE
  /\ wake = FALSE
  /\ acceptsAfterClose = 0
  /\ acceptsAfterHealth = 0
  /\ stage = [i \in Ids |-> 0]
  /\ acceptCount = [i \in Ids |-> 0]
  /\ chosenIds = {}
  /\ reclaimLog = << >>
  /\ execRevived = [s \in Slots |-> FALSE]

ReleasePhase(s) ==
  IF gen[s] = GenMax
    THEN [phase EXCEPT ![s] = "retired"]
    ELSE [phase EXCEPT ![s] = "free"]

AdvanceGen(s) ==
  IF gen[s] = GenMax \/ MutGenWrap
    THEN gen
    ELSE [gen EXCEPT ![s] = gen[s] + 1]

RetireSlot(s) ==
  /\ phase' = ReleasePhase(s)
  /\ gen' = AdvanceGen(s)
  /\ bind' = [bind EXCEPT ![s] = FALSE]
  /\ term' = [term EXCEPT ![s] = "none"]
  /\ exec' = [exec EXCEPT ![s] = 0]
  /\ ctl' = [ctl EXCEPT ![s] = 0]
  /\ pub' = [pub EXCEPT ![s] = "none"]
  /\ claimed' = [claimed EXCEPT ![s] = FALSE]
  /\ intent' = [intent EXCEPT ![s] = FALSE]
  /\ choices' = [choices EXCEPT ![s] = << >>]

Reserve(s) ==
  /\ phase[s] = "free"
  /\ ~closed
  /\ ~health \/ MutIgnoreHealth
  /\ phase' = [phase EXCEPT ![s] = "reserved"]
  /\ wake' = IF MutLazyReclaim THEN TRUE ELSE wake
  /\ UNCHANGED <<gen, bind, term, exec, ctl, pub, claimed, intent, choices,
                 closed, health, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, chosenIds, reclaimLog>>

Rollback(s) ==
  /\ phase[s] = "reserved" \/ (MutRollbackResidue /\ phase[s] = "accepted")
  /\ IF MutRollbackResidue /\ phase[s] = "accepted"
       THEN /\ phase' = ReleasePhase(s)
            /\ gen' = AdvanceGen(s)
            /\ UNCHANGED <<bind, term, exec, ctl, pub, claimed, intent, choices>>
       ELSE RetireSlot(s)
  /\ UNCHANGED <<closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, chosenIds, reclaimLog>>

CloseAdmission ==
  /\ ~closed
  /\ closed' = TRUE
  /\ UNCHANGED <<phase, gen, bind, term, exec, ctl, pub, claimed, intent, choices,
                 health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, chosenIds, reclaimLog>>

NoteHealth ==
  /\ ~health
  /\ health' = TRUE
  /\ UNCHANGED <<phase, gen, bind, term, exec, ctl, pub, claimed, intent, choices,
                 closed, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, chosenIds, reclaimLog>>

Accept(s) ==
  /\ phase[s] = "reserved"
  /\ ~closed \/ MutIgnoreClose
  /\ ~health \/ MutIgnoreHealth
  /\ phase' = [phase EXCEPT ![s] = "accepted"]
  /\ bind' = [bind EXCEPT ![s] = TRUE]
  /\ exec' = [exec EXCEPT ![s] = 1]
  /\ term' = [term EXCEPT ![s] = "none"]
  /\ pub' = [pub EXCEPT ![s] = "none"]
  /\ claimed' = [claimed EXCEPT ![s] = FALSE]
  /\ intent' = [intent EXCEPT ![s] = FALSE]
  /\ acceptCount' = [acceptCount EXCEPT ![Identity(s)] =
                       acceptCount[Identity(s)] + 1]
  /\ stage' = [stage EXCEPT ![Identity(s)] = 1]
  /\ acceptsAfterClose' = IF closed THEN 1 ELSE acceptsAfterClose
  /\ acceptsAfterHealth' = IF health THEN 1 ELSE acceptsAfterHealth
  /\ UNCHANGED <<gen, ctl, choices, closed, health, wake, chosenIds, reclaimLog,
                 execRevived>>

ClaimExec(s) ==
  /\ phase[s] = "accepted"
  /\ ~claimed[s]
  /\ term[s] = "none"
  /\ claimed' = [claimed EXCEPT ![s] = TRUE]
  /\ UNCHANGED <<phase, gen, bind, term, exec, ctl, pub, intent, choices,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, chosenIds, reclaimLog>>

PhysicalOutcome(s) ==
  /\ phase[s] = "accepted"
  /\ claimed[s]
  /\ term[s] = "none" \/ MutTerminalOverwrite
  /\ ~MutStrandPostAccept \/ ~intent[s]
  /\ \E v \in {"ok", "err"} :
       /\ term' = [term EXCEPT ![s] = v]
       /\ choices' = [choices EXCEPT ![s] =
                        Append(choices[s], [gen |-> gen[s], claimed |-> TRUE,
                                            kind |-> "phys"])]
  /\ chosenIds' = chosenIds \cup {Identity(s)}
  /\ intent' = [intent EXCEPT ![s] = FALSE]
  /\ UNCHANGED <<phase, gen, bind, exec, ctl, pub, claimed,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, reclaimLog>>

StaleTerminal(s, g) ==
  /\ MutStaleEvent
  /\ g < gen[s]
  /\ phase[s] = "accepted"
  /\ term[s] = "none"
  /\ term' = [term EXCEPT ![s] = "err"]
  /\ chosenIds' = chosenIds \cup {[slot |-> s, gen |-> g]}
  /\ choices' = [choices EXCEPT ![s] =
                   Append(choices[s], [gen |-> g, claimed |-> claimed[s],
                                       kind |-> "phys"])]
  /\ UNCHANGED <<phase, gen, bind, exec, ctl, pub, claimed, intent,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, reclaimLog>>

CancelRequest(s) ==
  /\ phase[s] = "accepted"
  /\ bind[s]
  /\ term[s] = "none"
  /\ IF claimed[s] /\ ~MutCancelAsPhysical
       THEN /\ intent' = [intent EXCEPT ![s] = TRUE]
            /\ UNCHANGED <<term, choices, chosenIds>>
       ELSE /\ term' = [term EXCEPT ![s] = "canceled"]
            /\ chosenIds' = chosenIds \cup {Identity(s)}
            /\ choices' = [choices EXCEPT ![s] =
                             Append(choices[s], [gen |-> gen[s],
                                                 claimed |-> claimed[s],
                                                 kind |-> "cancelwin"])]
            /\ intent' = [intent EXCEPT ![s] = FALSE]
  /\ UNCHANGED <<phase, gen, bind, exec, ctl, pub, claimed,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, reclaimLog>>

RetireExec(s) ==
  /\ phase[s] = "accepted"
  /\ exec[s] > 0
  /\ MutRetireLastExec \/ ~(exec[s] = 1 /\ term[s] = "none")
  /\ exec' = [exec EXCEPT ![s] = IF MutDoubleDecrement THEN exec[s] - 2 ELSE exec[s] - 1]
  /\ UNCHANGED <<phase, gen, bind, term, ctl, pub, claimed, intent, choices,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, chosenIds, reclaimLog>>

AcquireExec(s) ==
  /\ phase[s] = "accepted"
  /\ term[s] = "none" \/ MutExecAfterTerminal
  /\ exec[s] < MaxExec
  /\ exec' = [exec EXCEPT ![s] = exec[s] + 1]
  /\ execRevived' = [execRevived EXCEPT ![s] = execRevived[s] \/ (term[s] # "none")]
  /\ UNCHANGED <<phase, gen, bind, term, ctl, pub, claimed, intent, choices,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth,
                 stage, acceptCount, chosenIds, reclaimLog>>

AcquireCtl(s) ==
  /\ phase[s] = "accepted"
  /\ MutCtlAfterSettled \/ ~(pub[s] = "done" /\ ~bind[s] /\ exec[s] = 0)
  /\ ctl[s] < MaxCtl
  /\ ctl' = [ctl EXCEPT ![s] = ctl[s] + 1]
  /\ UNCHANGED <<phase, gen, bind, term, exec, pub, claimed, intent, choices,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, chosenIds, reclaimLog>>

RetireCtl(s) ==
  /\ phase[s] = "accepted"
  /\ ctl[s] > 0
  /\ ctl' = [ctl EXCEPT ![s] = IF MutDoubleDecrement THEN ctl[s] - 2 ELSE ctl[s] - 1]
  /\ UNCHANGED <<phase, gen, bind, term, exec, pub, claimed, intent, choices,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, chosenIds, reclaimLog>>

BeginPublish(s) ==
  /\ phase[s] = "accepted"
  /\ bind[s]
  /\ pub[s] = "none"
  /\ exec[s] = 0 \/ MutPublishWhileE
  /\ term[s] # "none" \/ MutReadyBeforePayload
  /\ pub' = [pub EXCEPT ![s] = "inflight"]
  /\ UNCHANGED <<phase, gen, bind, term, exec, ctl, claimed, intent, choices,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 stage, acceptCount, chosenIds, reclaimLog>>

CompletePublish(s) ==
  /\ pub[s] = "inflight"
  /\ pub' = [pub EXCEPT ![s] = "done"]
  /\ stage' = [stage EXCEPT ![Identity(s)] =
                 IF stage[Identity(s)] < 2 THEN 2 ELSE stage[Identity(s)]]
  /\ UNCHANGED <<phase, gen, bind, term, exec, ctl, claimed, intent, choices,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 acceptCount, chosenIds, reclaimLog>>

ReleaseBind(s) ==
  /\ phase[s] = "accepted"
  /\ bind[s]
  /\ term[s] # "none"
  /\ exec[s] = 0
  /\ pub[s] # "none"
  /\ stage' = [stage EXCEPT ![Identity(s)] =
                 IF stage[Identity(s)] < 3 THEN 3 ELSE stage[Identity(s)]]
  /\ IF MutReleaseResolvable
       THEN UNCHANGED bind
       ELSE bind' = [bind EXCEPT ![s] = FALSE]
  /\ UNCHANGED <<phase, gen, term, exec, ctl, pub, claimed, intent, choices,
                 closed, health, wake, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 acceptCount, chosenIds, reclaimLog>>

Reclaim(s) ==
  /\ phase[s] = "accepted"
  /\ ~bind[s]
  /\ exec[s] = 0
  /\ (IF MutReclaimIgnoresPub THEN TRUE ELSE pub[s] = "done")
  /\ (IF MutReclaimIgnoresCtl THEN TRUE ELSE ctl[s] = 0)
  /\ ~MutLazyReclaim \/ wake
  /\ reclaimLog' = Append(reclaimLog,
                          [id |-> Identity(s), exec |-> exec[s], ctl |-> ctl[s],
                           bind |-> bind[s], pub |-> pub[s]])
  /\ stage' = [stage EXCEPT ![Identity(s)] = 4]
  /\ RetireSlot(s)
  /\ IF MutLazyReclaim THEN wake' = FALSE ELSE UNCHANGED wake
  /\ UNCHANGED <<closed, health, acceptsAfterClose, acceptsAfterHealth, execRevived,
                 acceptCount, chosenIds>>

SlotAction(s) ==
  \/ Reserve(s)
  \/ Rollback(s)
  \/ Accept(s)
  \/ ClaimExec(s)
  \/ PhysicalOutcome(s)
  \/ CancelRequest(s)
  \/ RetireExec(s)
  \/ AcquireExec(s)
  \/ AcquireCtl(s)
  \/ RetireCtl(s)
  \/ BeginPublish(s)
  \/ CompletePublish(s)
  \/ ReleaseBind(s)
  \/ Reclaim(s)
  \/ \E g \in 0..(gen[s] - 1) : StaleTerminal(s, g)

Next ==
  \/ CloseAdmission
  \/ NoteHealth
  \/ \E s \in Slots : SlotAction(s)

Spec == Init /\ [][Next]_vars

ClaimExecAny == \E s \in Slots : ClaimExec(s)
PhysicalOutcomeAny == \E s \in Slots : PhysicalOutcome(s)
RetireExecAny == \E s \in Slots : RetireExec(s)
BeginPublishAny == \E s \in Slots : BeginPublish(s)
CompletePublishAny == \E s \in Slots : CompletePublish(s)
ReclaimAny == \E s \in Slots : Reclaim(s)

Fair ==
  /\ WF_vars(ClaimExecAny)
  /\ WF_vars(PhysicalOutcomeAny)
  /\ WF_vars(RetireExecAny)
  /\ WF_vars(BeginPublishAny)
  /\ WF_vars(CompletePublishAny)
  /\ WF_vars(ReclaimAny)

FairSpec == Spec /\ Fair

(*******************************************************************)
(* Safety                                                          *)
(*******************************************************************)

TypeOK ==
  /\ phase \in [Slots -> Phases]
  /\ gen \in [Slots -> 0..GenMax]
  /\ bind \in [Slots -> BOOLEAN]
  /\ term \in [Slots -> Terms]
  /\ exec \in [Slots -> 0..MaxExec]
  /\ ctl \in [Slots -> 0..MaxCtl]
  /\ pub \in [Slots -> Pubs]
  /\ claimed \in [Slots -> BOOLEAN]
  /\ intent \in [Slots -> BOOLEAN]
  /\ closed \in BOOLEAN
  /\ health \in BOOLEAN
  /\ wake \in BOOLEAN
  /\ acceptsAfterClose \in {0, 1}
  /\ acceptsAfterHealth \in {0, 1}
  /\ stage \in [Ids -> Stages]
  /\ acceptCount \in [Ids -> 0..2]
  /\ chosenIds \subseteq Ids
  /\ execRevived \in [Slots -> BOOLEAN]
  /\ Len(reclaimLog) =< 8
  /\ \A i \in 1..Len(reclaimLog) : reclaimLog[i] \in ReclaimRecords
  /\ \A s \in Slots : Len(choices[s]) =< 4

InvUnoccupiedClean ==
  \A s \in Slots :
    (phase[s] # "accepted") => UnoccupiedClean(s)

InvSingleAcceptance ==
  \A i \in Ids : acceptCount[i] =< 1

InvAcceptedRepresented ==
  \A i \in Ids :
    (acceptCount[i] >= 1) =>
      \/ (phase[i.slot] = "accepted" /\ gen[i.slot] = i.gen)
      \/ (stage[i] >= 3 /\ gen[i.slot] > i.gen)
      \/ (stage[i] >= 3 /\ phase[i.slot] = "retired" /\ gen[i.slot] = i.gen)

InvSingleWinner ==
  \A s \in Slots :
    \A i, j \in 1..Len(choices[s]) :
      (i < j) => (choices[s][i].gen < choices[s][j].gen)

InvTerminalMatchesGeneration ==
  \A s \in Slots :
    /\ \A i \in 1..Len(choices[s]) : choices[s][i].gen =< gen[s]
    /\ (phase[s] = "accepted" /\ Len(choices[s]) > 0) =>
         choices[s][Len(choices[s])].gen = gen[s]

InvCancelWinRequiresUnclaimed ==
  \A s \in Slots :
    \A i \in 1..Len(choices[s]) :
      choices[s][i].kind = "cancelwin" => ~choices[s][i].claimed

InvNoPubWhileExec ==
  \A s \in Slots :
    (pub[s] # "none") => (exec[s] = 0)

InvPublishedBacked ==
  \A i \in Ids : (stage[i] >= 2) => (i \in chosenIds)

InvReleasedNotLive ==
  \A i \in Ids :
    (stage[i] >= 3) =>
      ~((phase[i.slot] = "accepted") /\ (gen[i.slot] = i.gen) /\ bind[i.slot])

InvReclaimedNotLive ==
  \A i \in Ids :
    (stage[i] = 4) => ~((phase[i.slot] = "accepted") /\ (gen[i.slot] = i.gen))

InvReclaimConditions ==
  \A i \in 1..Len(reclaimLog) :
    /\ reclaimLog[i].exec = 0
    /\ reclaimLog[i].ctl = 0
    /\ ~reclaimLog[i].bind
    /\ reclaimLog[i].pub = "done"

InvNoAcceptAfterClose ==
  acceptsAfterClose = 0

InvNoAcceptAfterHealth ==
  acceptsAfterHealth = 0

InvExecHeldUntilTerminal ==
  \A s \in Slots :
    (phase[s] = "accepted" /\ term[s] = "none") => exec[s] > 0

InvNoExecRevival ==
  \A s \in Slots : ~execRevived[s]

(*******************************************************************)
(* Conditional liveness, by full request identity                  *)
(*******************************************************************)

ReclaimableId(i) ==
  /\ stage[i] >= 3
  /\ phase[i.slot] = "accepted"
  /\ gen[i.slot] = i.gen
  /\ Reclaimable(i.slot)

L1 ==
  \A i \in Ids :
    [](stage[i] >= 1 => <>(stage[i] >= 2))

L5 ==
  \A i \in Ids :
    [](ReclaimableId(i) => <>(stage[i] = 4))

=============================================================================
