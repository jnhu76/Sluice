------------------------------- MODULE SpawnWakeEpoch -------------------------------
(* SpawnWakeEpoch - spawn-to-busy-worker wake-epoch obligation (MODEL-007d).

Focused safety model for the #115 protocol: a runnable ticket published
onto a BUSY worker's local queue must publish the Scheduler wake
obligation (advance wake_epoch_), because a peer worker that has already
committed the unbounded wake-domain park can observe a cross-worker
publication ONLY through the epoch predicate (its cv predicate checks
epoch / terminate / OWN inbox - another worker's queue is invisible).

Two protection layers, each with an exact historical defect:
  1. Publication signals the epoch. Historical defect: #115 pre-fix
     (spawn/spawn_on pushed the ticket + inbox_cv.notify_one only - no
     waiter exists on inbox_cv, so the notify is inert).
  2. Park commit refuses while a stealable ticket exists
     (unguarded_progress_pending_locked, runnable-first). Historical
     defect: pre-G1 consumed-baseline stall (the baseline records the
     already-advanced epoch and the park sleeps through it).

C++ fact source (baseline c1e93f9):
  - src/async/scheduler.cpp:247-330   spawn()/spawn_on() (#115 RP-1/RP-2 fix)
  - src/async/scheduler_park_wake.cpp:128-157  signal_wake_locked
  - src/async/scheduler_park_wake.cpp:160-420  park commit (arm-then-recheck,
                                      baseline, cv predicate)
  - src/async/scheduler.cpp:1856-1894 unguarded_progress_pending_locked
  - tests/issue115_runnable_publication_wake_test.cpp (deterministic bridge)

Boundary: W0 = park candidate / thief; W1 = pinned busy inside user fiber
code (environment constant - the #115 precondition: a running observer
that can never drain its queue). One spawned ticket B. Safety/accounting
only; no fairness, no liveness claim. Non-goals: W1 completion escape
hatch, self-targeted delivery (the own-inbox predicate backstop - e9's
domain), terminate clause, idle dance (#161's own suite), deadlines,
backend observation, the Phase G bridge, the refuse-branch signal (its
consumer would be a third worker - out of boundary).

inboxNotified mirrors the C++ fact that inbox_cv.notify_one is
unconditional and has NO consumer (issue #170): it is an observable that
nothing reads - which is exactly why NEG-SP2 ("notify wrong transport
only") is representationally identical to NEG-SP1 and is not a separate
gate.

Memory-model boundary: wake_epoch_ lives entirely under wake_mtx_ (SC
mutex domain); the modeled protocol has no lock-free atomics. TLC proves
the SC abstraction only - no C++ weak-memory claim.

Init uses equality form (TLC 2.19 initial-state enumerator rejects
negation-form constraints; issue #172 lesson). *)
EXTENDS Naturals

CONSTANTS
    SignalOnPublication,  \* NEG-SP1: FALSE = the #115 pre-fix shape (no epoch advance)
    RecheckEnforced       \* NEG-SP4: FALSE = pre-G1 commit (baseline consumes the signal)

VARIABLES
    w0State,         \* Looping / Parked
    ticketB,         \* None / OnW1 / OnW0 / Consumed
    wakeEpoch,       \* Scheduler wake_epoch_ (bounded: one publication advance)
    observedEpochW0, \* the parked baseline (ws->observed_epoch)
    inboxNotified,   \* inbox_cv notify observable - set by every publication,
                     \* consumed by nothing (no C++ waiter exists, issue #170)
    sawParkRefuse,   \* history witness: the commit recheck refused
    wokeFromPark     \* history witness: the cv predicate fired on this park

vars == <<w0State, ticketB, wakeEpoch, observedEpochW0, inboxNotified,
          sawParkRefuse, wokeFromPark>>

Init ==
    /\ w0State = "Looping"
    /\ ticketB = "None"
    /\ wakeEpoch = 0
    /\ observedEpochW0 = 0
    /\ inboxNotified = FALSE
    /\ sawParkRefuse = FALSE
    /\ wokeFromPark = FALSE

(* P1 - the fused G-scope publication (spawn/spawn_on/route): ticket onto
   the busy target's queue + the (inert) inbox notify + the epoch advance.
   Fusing push+signal is faithful: both run under one global_mtx_ scope in
   the C++, and the park commit (also G) cannot interleave - which is also
   why "signal before push" has no interleaving window (NEG-SP3 dropped). *)
SpawnB ==
    /\ ticketB = "None"
    /\ ticketB' = "OnW1"
    /\ inboxNotified' = TRUE
    /\ wakeEpoch' = IF SignalOnPublication THEN wakeEpoch + 1 ELSE wakeEpoch
    /\ UNCHANGED <<w0State, observedEpochW0, sawParkRefuse, wokeFromPark>>

(* K1 - the G1 park commit, arm-then-recheck: the baseline may be armed
   only when no live stealable ticket exists (ticket absent or already
   consumed). NEG-SP4: the mutant arms the baseline regardless, so a
   pre-publication epoch advance is consumed by the baseline. *)
Park ==
    /\ w0State = "Looping"
    /\ \/ ticketB \in {"None", "Consumed"}
       \/ RecheckEnforced = FALSE
    /\ w0State' = "Parked"
    /\ observedEpochW0' = wakeEpoch
    /\ UNCHANGED <<ticketB, wakeEpoch, inboxNotified, sawParkRefuse,
                   wokeFromPark>>

(* K1-refuse witness: the recheck refuses while a stealable ticket exists;
   the worker stays looping and steals at loop top. Witness-only ghost. *)
ParkRefuse ==
    /\ w0State = "Looping"
    /\ ticketB = "OnW1"
    /\ RecheckEnforced = TRUE
    /\ sawParkRefuse' = TRUE
    /\ UNCHANGED <<w0State, ticketB, wakeEpoch, observedEpochW0,
                   inboxNotified, wokeFromPark>>

(* K2 - the cv predicate firing: an epoch advance past the baseline wakes
   the parked worker (state-first-then-signal closes the commit-to-sleep
   race; the epoch IS the persistent wake state). *)
WakeObserve ==
    /\ w0State = "Parked"
    /\ wakeEpoch # observedEpochW0
    /\ w0State' = "Looping"
    /\ wokeFromPark' = TRUE
    /\ UNCHANGED <<ticketB, wakeEpoch, observedEpochW0, inboxNotified,
                   sawParkRefuse>>

(* S1 - steal: transport, not publication (E8's domain; fused move+owner
   transfer). Only a looping worker transports. *)
StealB ==
    /\ w0State = "Looping"
    /\ ticketB = "OnW1"
    /\ ticketB' = "OnW0"
    /\ UNCHANGED <<w0State, wakeEpoch, observedEpochW0, inboxNotified,
                   sawParkRefuse, wokeFromPark>>

ConsumeB ==
    /\ w0State = "Looping"
    /\ ticketB = "OnW0"
    /\ ticketB' = "Consumed"
    /\ UNCHANGED <<w0State, wakeEpoch, observedEpochW0, inboxNotified,
                   sawParkRefuse, wokeFromPark>>

Stutter == UNCHANGED vars

Next ==
    \/ SpawnB
    \/ Park
    \/ ParkRefuse
    \/ WakeObserve
    \/ StealB
    \/ ConsumeB
    \/ Stutter

Spec == Init /\ [][Next]_vars

(* ---- As-built safety laws ---- *)

(* The wake obligation: a committed-parked peer and a cross-worker
   published ticket can coexist ONLY if the publication advanced the epoch
   past the parked baseline. The violating state IS the persistent
   stranded shape: the predicate can never fire and nothing else consumes
   the ticket (W1 is pinned). *)
InvWakeObligation ==
    (w0State = "Parked" /\ ticketB = "OnW1") => (wakeEpoch > observedEpochW0)

(* The baseline never exceeds the current epoch. *)
InvBaselineSound ==
    observedEpochW0 \leq wakeEpoch

(* Transport only by a looping worker. Rests on the same commit recheck:
   under NEG-SP4 this is an entailed co-victim (a recheck-less commit can
   park the worker on top of its own stolen ticket - the pre-G1 shape). *)
InvStealRequiresAwake ==
    (ticketB = "OnW0") => (w0State # "Parked")

(* No consumption without a publication: the ticket only ever reaches
   Consumed through the published OnW1 location (SpawnB sets the
   publication observable). *)
InvConsumedRequiresPublication ==
    (ticketB = "Consumed") => inboxNotified

(* ---- Reachability witnesses (NoReach* deliberately false at the target;
   TLC's CEX is the witness). ---- *)

NoReachParked == w0State # "Parked"
NoReachPublishedWhileParked == ~ (w0State = "Parked" /\ ticketB = "OnW1")
NoReachEpochAdvanced == ~ (wakeEpoch = 1 /\ ticketB # "None")
NoReachRescued == ticketB # "Consumed"
NoReachRescuedAfterWake == ~ (wokeFromPark /\ ticketB = "Consumed")
NoReachParkRefuse == ~ sawParkRefuse
====
