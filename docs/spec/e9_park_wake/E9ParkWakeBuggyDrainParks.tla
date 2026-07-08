------------------------------- MODULE E9ParkWakeBuggyDrainParks -------------------------------
(*
  E9-CORRECTIVE negative model: the shipped Drain-park defect.

  This module EXTENDS E9ParkWake (the corrected model) and redefines the
  minimal action set to introduce ONE semantic defect: the idle-action
  selection admits a Drain MW-S3 park when an external-wake-capable wait
  is registered, and the physical park is signal-only (no guaranteed
  bounded observation return). This is exactly the shipped E9 defect:
  external_wake_possible_locked() was used as the MW-S3 run-lifetime
  decision (the semantic conflation, ADR §9.4.0 / audit L4), and the
  park predicate had no observation-return authority.

  Correct protocol:
      Drain + MW-S3  -> ReturnStalled
  Buggy protocol (this model):
      Drain + MW-S3 + ExternalWakeCapable  -> Park  (and strand)

  The minimal defect has TWO facets, both present in the shipped code:
    (1) ParkAdmittedBuggy: admits Drain+MW-S3+external-wake park.
    (2) LeavePark signal-only: no guaranteed observation return, so a
        parked worker with no wake-relevant publication due is STRANDED.

  Required counterexample (configured in .cfg via Life2Buggy):
  a Drain run reaches MW-S3 with an external-wake-capable wait, a Worker
  commits + enters physical park, and (with NO producer acting) the run
  NEVER reaches a terminal state -- it is stuck with a Parked worker
  under MW-S3. This violates the corrected E9-LIFE-2.

  The counterexample does NOT assume the external producer eventually
  completes, the backend eventually completes, or shutdown occurs.
*)
EXTENDS E9ParkWake

(* BUGGY park admission: admits the Drain MW-S3 park when an external-
   wake-capable wait is registered (the semantic conflation). *)
ParkAdmittedBuggy ==
    /\ runState = "Active"
    /\ \/ GlobalClass = "MWS2"
       \/ GlobalClass = "MWS3"   \* BUG: MW-S3 park admitted in BOTH modes
          /\ ExternalWakePossible

(* BUGGY candidate admission uses the buggy rule. *)
BeginParkCandidateBuggy(w) ==
    /\ workerPhase[w] = "Active"
    /\ runState = "Active"
    /\ ParkAdmittedBuggy
    /\ ~ExecutableWork
    /\ ~LatentExternalWork
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "ParkCandidate"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch, backendWaitParticipant,
                   runMode, runState>>

FinalParkRecheckAndCommitBuggy(w) ==
    /\ runState = "Active"
    /\ workerPhase[w] = "ParkCandidate"
    /\ ParkAdmittedBuggy
    /\ ~ExecutableWork
    /\ ~LatentExternalWork
    /\ observedEpoch' = [observedEpoch EXCEPT ![w] = wakeEpoch]
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "ParkCommitted"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, backendWaitParticipant, runMode, runState>>

(* BUGGY LeavePark: signal-only (no guaranteed observation return). With
   no producer acting, a parked worker under MW-S3 is STRANDED. *)
LeaveParkBuggy(w) ==
    /\ runState = "Active"
    /\ workerPhase[w] = "Parked"
    /\ (\/ wakeEpoch # observedEpoch[w]
        \/ backendReady
        \/ ExecutableWork
        \/ runnableVisible
        \/ LatentExternalWork)
    /\ IF backendWaitParticipant = w
       THEN /\ backendWaitParticipant' = NONE
       ELSE /\ backendWaitParticipant' = backendWaitParticipant
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch, runMode, runState>>

(* BUGGY EnterPhysicalPark: no Drain/MW-S3 idle-action re-selection. *)
EnterPhysicalParkBuggy(w) ==
    /\ runState = "Active"
    /\ workerPhase[w] = "ParkCommitted"
    /\ IF /\ wakeEpoch = observedEpoch[w]
          /\ ~backendReady
          /\ ~LatentExternalWork
          /\ ~ExecutableWork
       THEN
          /\ IF /\ MWS2
                /\ ~ExternalWakePossible
                /\ backendWaitParticipant = NONE
             THEN /\ backendWaitParticipant' = w
                  /\ workerPhase' = [workerPhase EXCEPT ![w] = "Parked"]
             ELSE /\ backendWaitParticipant' = backendWaitParticipant
                  /\ workerPhase' = [workerPhase EXCEPT ![w] = "Parked"]
       ELSE
          /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
          /\ backendWaitParticipant' = backendWaitParticipant
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch, runMode, runState>>

(* Buggy Next: same as the correct Next but the park actions + LeavePark
   are the buggy variants. Everything else (producers, fiber lifecycle,
   ReturnStalled/ReturnQuiescent/Shutdown) is inherited unchanged. *)
NextBuggy ==
    \/ ExternalReadyPublish
    \/ DrainExternalReady
    \/ SubmitBackend
    \/ BackendReadyPublish
    \/ DrainBackendReady
    \/ PublishRunnable
    \/ \E w \in Workers : BeginParkCandidateBuggy(w)
    \/ \E w \in Workers : FinalParkRecheckAndCommitBuggy(w)
    \/ \E w \in Workers : AbandonParkCandidate(w)
    \/ \E w \in Workers : EnterPhysicalParkBuggy(w)
    \/ \E w \in Workers : LeaveParkBuggy(w)
    \/ ReturnStalled
    \/ ReturnQuiescent
    \/ RunFiber
    \/ SuspendFiber
    \/ FinishFiber
    \/ ShutdownSignal
    \/ TerminalStutter

SpecBuggy == Init /\ [][NextBuggy]_vars

(* Fairness: WF on the buggy LeavePark (signal-only) and on ReturnStalled.
   No producer/backend/shutdown fairness. *)
FairLeaveParkBuggy ==
    \A w \in Workers : WF_vars (LeaveParkBuggy(w))

LivenessSpecBuggy ==
    SpecBuggy
    /\ FairLeaveParkBuggy
    /\ WF_vars (ReturnStalled)
    /\ WF_vars (ReturnQuiescent)
    /\ \A w \in Workers : WF_vars (AbandonParkCandidate(w))
    /\ \A v \in Workers : WF_vars (EnterPhysicalParkBuggy(v))

(* The property under test: the corrected E9-LIFE-2 says a Drain MW-S3
   must end the run (or leave MW-S3 via progress). Under the BUGGY
   model, a Drain MW-S3 with an external-wake-capable wait can park and
   STRAND (no producer acts), so this property FAILS -- the
   counterexample reproduces the deterministic hang. *)
Life2Buggy ==
    [] ( (runMode = "Drain" /\ GlobalClass = "MWS3" /\ runState = "Active")
          => <> (runState # "Active" \/ GlobalClass # "MWS3") )

=============================================================================
