------------------------------- MODULE E12SchedLiveness -------------------------------
(*
  E12 x Scheduler combined LIVENESS model (issue #161).

  SCOPE — the T22 multi-worker hang scenario, end to end:

    tests/async_rwlock_test.cpp rwlock_mw_cancel_and_unlock_on_different_workers
    (CI PR #160 hang: both workers parked in the unbounded wake-domain cv wait,
    Scheduler::run_impl blocked in thread::join, RunMode::drain, workers=2).

    The three C++ run() invocations are collapsed into the initial state:

      run 1 (writer acquires, parks on the ready flag)   -> Init
      run 2 (R1, R2 queue behind the writer)             -> Init
      between-run cancel(rn1) from the main thread       -> Init (R1's node
        already Cancelled; R1's fiber holds a runnable ticket distributed to
        W0's inbox by run 3's setup; rwlock queue = <<R2>>)
      run 3 (flag already TRUE at entry)                 -> the modeled run

    Fiber programs at run-3 entry (bounded, finite — application fairness is
    an assumption of the model, matching the test's finite fibers):

      WF: FlagWait --(drain publishes ticket)--> Ticket --(pop/run)-->
          RunUnlock [G-atomic: clear writer, claim R2, activeReaders:=1,
                     publish R2 ticket to W1, signal, erase idle] --> Done
      R1: Ticket --(pop/run)--> RunNoop --> Done   (cancelled waiter resume;
          the fiber body does nothing after read_lock returns)
      R2: QueueWait --(WF's grant)--> Ticket --> RunFlag [r2Acquired:=TRUE]
          --> RunUnlockRead [activeReaders:=0; queue empty, no grant] --> Done

  THE LOAD-BEARING QUESTION (issue #161):

    After all work completes, does the two-worker idle dance converge to
    global_terminate so run_impl's join returns?

  ROOT-CAUSE HYPOTHESIS (proven or refuted by TLC below):

    The not-last dancer's idle contribution can be ERASED by the unlocked
    pop-path idle reset (scheduler.cpp:550, no mandatory follow-up signal)
    between the dancer's contribution and its park commit. The R4 backstop
    (`idle_workers_ > idle_dance_contributed_`, commit 17907b1) cannot
    distinguish the dancer's STALE 1-bit contribution flag from the eraser's
    FRESH contribution, and the eraser's not-last signal can be absorbed by
    the dancer's still-unarmed park baseline. Both workers then sleep in the
    unbounded park with idle < live and no future signal: the join hangs
    even though every fiber completed.

  REPAIR UNDER TEST (CONSTANT RepairContributionGeneration):

    Abstract law, not an implementation: an idle-count reset that can orphan
    an outstanding contribution invalidates the IDENTITY (generation) of all
    live contributions; a dancer may arm its park baseline only while its
    own contribution is still current, else it refuses, signals, and
    re-loops (the re-dance then sees the eraser's fresh contribution and
    converges: prev+1 >= live -> LAST -> terminate). The C++ refinement
    (which reset sites are genuine invalidation events) is decided AFTER
    the model evidence (issue #161 B4 site classification).

  MUTANTS (single-defect toggles; default FALSE):

    M1NoPubSignal   M1: publication routes a ticket without advancing the
                    wake epoch (the issue-#115 shape). NOTE: in this closed
                    scenario the own-inbox predicate backstop (E9 Section 10)
                    plus steal cover the class; the expected verdict is
                    DOCUMENTED PASS — see README (the hazard needs an
                    unbounded busy owner, out of the T22 finite-fiber scope;
                    e9 BuggyPrePark carries the class at its abstraction).
    M2NoTransport   M2: pure-baseline transport — the cv predicate drops the
                    own-inbox clause AND the park commit drops the progress
                    scan (the pre-G1/pre-E9-Section-10 shape). Expected:
                    NoStrandedRunnable counterexample.
    M3NoCommitRecheck M3: park commit arms the baseline without the progress
                    recheck (the pre-G1 arm-before-recheck shape; the
                    own-inbox backstop is KEPT). Expected in this closed
                    scenario: DOCUMENTED PASS — see README.
    M5GrantNoTicket M5: the grant claims/resolves R2's node and commits the
                    resource but publishes no runnable ticket. Expected:
                    CancelUnlockScenarioEventuallyCompletes counterexample
                    (run returns STALLED with r2Acquired false).

    M4 is not a toggle: M4 IS the as-built protocol
    (RepairContributionGeneration = FALSE).

  STATE AXES (all non-atomic C++ steps that matter are separate actions):

    workerStage[w]: LoopTop -> Popped -> PopBumpPending -> PopEraseDone ->
                        Executing -> LoopTop
                    LoopTop -> DrainClassify -> MwS1Idle -> MwS1BumpPending
                                             -> MwS1GRecheck -> ParkCandidate
                                                 -> DanceGo -> ParkCandidate
                    ParkCandidate -> Parked | LoopTop(refuse)
                    Parked -> LoopTop | Exited ; any -> Exited(terminate)
      Popped         models scheduler.cpp:513-517 (ticket removed from the
                   inbox under inbox_mtx only — INVISIBLE to classify and to
                   the park-commit progress scan).
      PopBumpPending models :578 executed (the unlocked exchange(0) has
                   landed) while :580 (the conditional generation bump) has
                   not — the SPLIT WINDOW between the eraser's two unlocked
                   RMWs, in which a stale contributor's park commit can read
                   the erased count together with the still-current
                   generation (see the split-window refinement below and the
                   E12SchedLivenessSplitWindow reachability witness).
      PopEraseDone  models :580 done but :1215 running_fiber_count_++ not
                   yet — the classify-blind and commit-scan-blind window
                   that admits a quiescent dance beside live work.
      MwS1Idle      models the mw_s1 fall-through between the (unlocked)
                   classify at :567 and the idle reset at :622.
      MwS1BumpPending models the same split window on the second erase site
                   (:622 exchange landed, :624 bump not yet).
    held[w]:        the popped ticket inside the pop window / executing.
    inbox[w]:       runnable tickets (fiber pc = "Ticket").
    wakeEpoch:      monotonic; every signal advances it.
    observedEpoch[w]: the park baseline (park_wake.cpp:321).
    idleCount / contributed[w] / contribGen[w] / contributionGen: the dance.
                   contributed[w] resets at every loop-top re-entry (C++
                   :509) and survives only inside one iteration — a parked
                   dancer sleeps HOLDING its contribution (the R4 damping).

  C++ ATOMICITY MAPPING (what is one action here and why):

    - global_mtx_ critical sections are single actions: the WF unlock grant
      (unlock_write holds G across clear+claim+commit+publish+signal), the
      drain+classify entry (one G section), the dance contribute+not-last
      signal (one G section, :942-1096).
    - The steps C++ performs OUTSIDE any serializing lock are separate
      actions: pop (:513), pop-erase (:578), running++ (:1215), the mw_s1
      fall-through erase (:622), park candidate->commit seam gap, the cv
      predicate wake.
    - SPLIT-WINDOW FIDELITY (review of the repair round): the two unlocked
      erase sites are each TWO actions — the exchange(0) and its
      conditional generation bump (BumpPopGen / BumpMwS1Gen) — because the
      C++ writes are two distinct unlocked RMWs a peer's park commit can
      land between. A park commit that reads the erased count with the
      still-current generation passes the identity term (witness gate
      E12SchedLivenessSplitWindow); the safety/liveness gates then prove
      that pass produces only a TRANSIENT park (the dichotomy argument in
      the suite README): the eraser's bump, fiber run, and re-dance
      not-last signal are all sequenced after the arming, so the cv
      predicate — not a lost signal — returns the dancer to the loop. The
      ParkCommit itself stays ONE action (recheck then arm inside one G
      section); its refusal verdict is a pure function of the (idleCount,
      contributionGen) pair it reads, and every pair the split C++ loads
      can produce (pre-erase, post-bump, and the split combination) is
      explored as a distinct state the atomic commit reads from.
    - The dance final re-check (:1039) is folded into DanceContribute (same
      G section in C++; only runningCount may change under it in C++, and
      the fold explores that as a pre-state difference because
      DrainClassify/DanceContribute are separate actions here — an
      over-approximation, never an under-approximation).
    - LoopTop fidelity: a worker at LoopTop always holds contributed=0
      (every entry path — TryPop, TrySteal, park refusal, park leave,
      fiber completion, and the :1065 reset-continue — models the C++
      loop-top flag reset at :513; the reset-continue originally left a
      stale contributed=1, over-approximating the identity-term refusals).
    - Steal (E8) is modeled as the LoopTop TrySteal action and
      DrainClassify REQUIRES nothing stealable, mirroring the C++
      straight-line order pop -> steal -> drain/classify. Without this the
      refuse/re-loop path would admit an infinite wake-epoch state cycle.

  Fairness: WF on every scheduler/worker action above. NO fairness on any
  producer (the ready flag is TRUE from Init — a state, not an event), and
  no ASSUME that any signal eventually happens.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Workers, Fibers, W0, W1, WF, R1, R2, NOFIBER,
          RepairContributionGeneration,
          BumpPopErase, BumpMwS1Erase, BumpRecheckErase,
          BumpPubErase, BumpDanceResetErase,
          M1NoPubSignal, M2NoTransport, M3NoCommitRecheck, M5GrantNoTicket

VARIABLES
    workerStage,
    held,
    inbox,
    fiberPC,
    writerActive,
    activeReaders,
    r2Queued,
    r2Acquired,
    waitingReady,
    runningCount,
    wakeEpoch,
    observedEpoch,
    idleCount,
    contributed,
    contributionGen,
    contribGen,
    terminate,
    resCount,
    pubCount

StageVal == {"LoopTop", "Popped", "PopBumpPending", "PopEraseDone", "Executing",
             "MwS1Idle", "MwS1BumpPending", "MwS1GRecheck", "DanceGo",
             "ParkCandidate", "Parked", "Exited"}
PCVal == {"FlagWait", "QueueWait", "Ticket", "RunUnlock", "RunNoop",
          "RunFlag", "RunUnlockRead", "Done"}

ASSUME
    /\ Workers = {W0, W1}
    /\ Fibers = {WF, R1, R2}
    /\ W0 # W1
    /\ WF # R1 /\ R1 # R2 /\ WF # R2
    /\ NOFIBER \notin Fibers
    /\ RepairContributionGeneration \in {TRUE, FALSE}
    /\ BumpPopErase \in {TRUE, FALSE}
    /\ BumpMwS1Erase \in {TRUE, FALSE}
    /\ BumpRecheckErase \in {TRUE, FALSE}
    /\ BumpPubErase \in {TRUE, FALSE}
    /\ BumpDanceResetErase \in {TRUE, FALSE}
    /\ M1NoPubSignal \in {TRUE, FALSE}
    /\ M2NoTransport \in {TRUE, FALSE}
    /\ M3NoCommitRecheck \in {TRUE, FALSE}
    /\ M5GrantNoTicket \in {TRUE, FALSE}

vars == <<workerStage, held, inbox, fiberPC, writerActive, activeReaders,
          r2Queued, r2Acquired, waitingReady, runningCount, wakeEpoch,
          observedEpoch, idleCount, contributed, contributionGen, contribGen,
          terminate, resCount, pubCount>>

(* Run-3 entry: flag TRUE, writer holds the lock and waits on the flag,
   R1 cancelled (its fiber holds the ticket distributed to W0), R2 queued. *)
Init ==
    /\ workerStage = [w \in Workers |-> "LoopTop"]
    /\ held = [w \in Workers |-> NOFIBER]
    /\ inbox = [v \in Workers |-> IF v = W0 THEN {R1} ELSE {}]
    /\ fiberPC = [f \in Fibers |-> CASE f = WF -> "FlagWait"
                                       [] f = R1 -> "Ticket"
                                       [] f = R2 -> "QueueWait"]
    /\ writerActive = TRUE
    /\ activeReaders = 0
    /\ r2Queued = TRUE
    /\ r2Acquired = FALSE
    /\ waitingReady = TRUE
    /\ runningCount = 0
    /\ wakeEpoch = 0
    /\ observedEpoch = [w \in Workers |-> 0]
    /\ idleCount = 0
    /\ contributed = [w \in Workers |-> 0]
    /\ contributionGen = 0
    /\ contribGen = [w \in Workers |-> 0]
    /\ terminate = FALSE
    /\ resCount = [f \in Fibers |-> IF f = R2 THEN 0 ELSE 1]
    /\ pubCount = [f \in Fibers |-> IF f = R2 THEN 0 ELSE 1]

(* =========================================================================
   Derived predicates
   ========================================================================= *)

LiveWorkers == Cardinality(Workers)

TicketVisible ==
    \E v \in Workers : inbox[v] # {}

StealableFrom(w) ==
    \E v \in Workers : v # w /\ inbox[v] # {}

ObserverExists ==
    runningCount > 0

(* The park-commit progress scan (unguarded_progress_pending_locked): a
   runnable ticket ANYWHERE refuses the park unless a running fiber (the
   observer exemption) may drain it. A HELD ticket (pop window) is INVISIBLE
   to the scan — faithful to the C++ (the scan reads inboxes and
   pending_spawn_, not other workers' local variables). *)
ProgressPending ==
    TicketVisible /\ ~ObserverExists

(* The cv predicate (park_wake.cpp:379-396): epoch mismatch, terminate, or
   the worker's OWN inbox (the E9 Section 10 backstop). M2 drops the
   own-inbox clause (and the commit scan below). *)
CanLeavePark(w) ==
    \/ wakeEpoch # observedEpoch[w]
    \/ terminate
    \/ (~M2NoTransport /\ inbox[w] # {})

(* The park-commit refusal condition (park_wake.cpp:299-300):
   (a) unguarded progress with no observer (G1 / issue #115; M3 drops it);
   (b) an idle-dance count this worker has not contributed to (R4 backstop);
   (c) [repair] this worker's LIVE contribution identity (contributed=1) was
       invalidated by an idle reset since it danced. The contributed=1 gate
       matters: a never-danced worker (loop-top reset, C++ :509) must not
       refuse behind a generation it never claimed — the observer-exemption
       park beside a running fiber is legitimate delegation. *)
RefusePark(w) ==
    \/ (~M3NoCommitRecheck /\ ~M2NoTransport /\ ProgressPending)
    \/ idleCount > contributed[w]
    \/ (RepairContributionGeneration /\ contributed[w] = 1
        /\ contributionGen # contribGen[w])

(* Idle reset with the repair's identity invalidation — the G-SECTION sites
   ONLY (the drain/route publication erase, the :958 reclassify erase, and
   the :1065 reset-continue erase): there the erase and its bump sit inside
   ONE global_mtx_ critical section with the accompanying signal/re-loop,
   so a single atomic action is the faithful granularity. The two UNLOCKED
   sites are NOT routed here — they are the split EraseIdleOnPop/BumpPopGen
   and EraseIdleMwS1/BumpMwS1Gen pairs, so the model explores the C++ split
   window between exchange(0) and the generation bump. The B4 site
   classification: each reset site routes through its own bump toggle, so
   model experiments can decide which sites are GENUINE invalidation events
   (disabling the bump must reproduce the M4 stuck state) and which are
   self-guarded by other protocol mechanisms (disabling the bump must stay
   safe). *)
EraseIdleBumping(bump) ==
    /\ idleCount' = 0
    /\ contributionGen' = IF (RepairContributionGeneration /\ bump)
                          THEN contributionGen + 1
                          ELSE contributionGen

(* =========================================================================
   Worker loop-top: pop, steal (E8 transport), drain+classify
   ========================================================================= *)

(* scheduler.cpp:513-517 — pop under inbox_mtx only. Entering a fresh loop
   iteration resets the dance contribution flag (C++ :509). *)
TryPop(w) ==
    /\ workerStage[w] = "LoopTop"
    /\ \E f \in inbox[w] :
        /\ held' = [held EXCEPT ![w] = f]
        /\ inbox' = [inbox EXCEPT ![w] = inbox[w] \ {f}]
        /\ workerStage' = [workerStage EXCEPT ![w] = "Popped"]
        /\ contributed' = [contributed EXCEPT ![w] = 0]
        /\ UNCHANGED <<fiberPC, writerActive, activeReaders, r2Queued,
                       r2Acquired, waitingReady, runningCount, wakeEpoch,
                       observedEpoch, idleCount, contributionGen,
                       contribGen, terminate, resCount, pubCount>>

(* E8 steal: MOVE one stealable ticket to w (transport, not publication).
   The C++ loop order pop -> steal -> drain makes DrainClassify(w) below
   require nothing stealable; `continue` re-enters the loop top (the
   contribution reset). *)
TrySteal(w) ==
    /\ workerStage[w] = "LoopTop"
    /\ inbox[w] = {}
    /\ \E v \in Workers : \E f \in inbox[v] :
        /\ v # w
        /\ held' = held
        /\ inbox' = [inbox EXCEPT ![v] = inbox[v] \ {f},
                                  ![w] = inbox[w] \cup {f}]
        /\ workerStage' = [workerStage EXCEPT ![w] = "LoopTop"]
        /\ contributed' = [contributed EXCEPT ![w] = 0]
        /\ UNCHANGED <<fiberPC, writerActive, activeReaders, r2Queued,
                       r2Acquired, waitingReady, runningCount, wakeEpoch,
                       observedEpoch, idleCount, contributionGen,
                       contribGen, terminate, resCount, pubCount>>

(* scheduler.cpp:578 — the UNLOCKED pop-path idle reset, STEP 1 of 2 (the
   exchange(0)). The conditional generation bump is a SEPARATE, arbitrarily-
   delayable action (BumpPopGen, :580): the C++ writes are two distinct
   unlocked RMWs, and a peer's park commit can land between them — the
   SPLIT WINDOW. The exchange itself knows whether it erased a contribution
   (idleCount > 0 at the RMW, the C++ erased != 0); the owed bump observes
   the ERASE-time answer, never the bump-time count. With the repair off
   (or the site's bump disabled by B4 experiment), no bump is owed and the
   stage advances directly — the pre-split (as-built) protocol. *)
EraseIdleOnPop(w) ==
    /\ workerStage[w] = "Popped"
    /\ idleCount' = 0
    /\ workerStage' = [workerStage EXCEPT ![w] =
                           IF RepairContributionGeneration /\ BumpPopErase
                              /\ idleCount > 0
                           THEN "PopBumpPending"
                           ELSE "PopEraseDone"]
    /\ UNCHANGED <<held, inbox, fiberPC, writerActive, activeReaders,
                   r2Queued, r2Acquired, waitingReady, runningCount,
                   wakeEpoch, observedEpoch, contributed, contributionGen,
                   contribGen, terminate, resCount, pubCount>>

(* scheduler.cpp:580 — the pop-path generation bump, STEP 2 of 2. Sequenced
   strictly after the exchange in the eraser's program order (StartFiber
   requires this stage to have cleared), and sequenced before the fiber's
   execution and every subsequent G-section progress of the eraser — the
   ordering the split-window dichotomy argument leans on. *)
BumpPopGen(w) ==
    /\ workerStage[w] = "PopBumpPending"
    /\ contributionGen' = contributionGen + 1
    /\ workerStage' = [workerStage EXCEPT ![w] = "PopEraseDone"]
    /\ UNCHANGED <<held, inbox, fiberPC, writerActive, activeReaders,
                   r2Queued, r2Acquired, waitingReady, runningCount,
                   wakeEpoch, observedEpoch, idleCount, contributed,
                   contribGen, terminate, resCount, pubCount>>

(* scheduler.cpp:1211-1219 — make_running + running_fiber_count_++. The
   ticket becomes observable to classify (running observer) only HERE. *)
StartFiber(w) ==
    /\ workerStage[w] = "PopEraseDone"
    /\ held[w] # NOFIBER
    /\ fiberPC[held[w]] = "Ticket"
    /\ fiberPC' = [fiberPC EXCEPT ![held[w]] =
                       CASE held[w] = WF -> "RunUnlock"
                       [] held[w] = R1   -> "RunNoop"
                       [] held[w] = R2   -> "RunFlag"]
    /\ runningCount' = runningCount + 1
    /\ workerStage' = [workerStage EXCEPT ![w] = "Executing"]
    /\ UNCHANGED <<held, inbox, writerActive, activeReaders, r2Queued,
                   r2Acquired, waitingReady, wakeEpoch, observedEpoch,
                   idleCount, contributed, contributionGen, contribGen,
                   terminate, resCount, pubCount>>

(* =========================================================================
   Drain + classify (one global_mtx_ section: scheduler.cpp:562-568 and
   940-955). The ready-flag drain is level-triggered (E5-A2): flag TRUE and
   WF registered -> publish WF's ticket to WF's owner (W0), signal, reset
   idle (the route-erase :1452 is in the SAME G section as the signal).
   Then classify: mw_s1 (ticket visible or a running fiber) -> the mw_s1
   fall-through stage; otherwise -> the dance stage.
   ========================================================================= *)
DrainClassify(w) ==
    /\ workerStage[w] = "LoopTop"
    /\ held[w] = NOFIBER
    /\ inbox[w] = {}
    /\ ~StealableFrom(w)
    /\ LET drainFired == waitingReady
           pubSignal == IF M1NoPubSignal THEN 0 ELSE 1
           inboxAfter == IF drainFired
                         THEN [inbox EXCEPT ![W0] = inbox[W0] \cup {WF}]
                         ELSE inbox
           pcAfter == IF drainFired
                      THEN [fiberPC EXCEPT ![WF] = "Ticket"]
                      ELSE fiberPC
           mwS1After == (\E v \in Workers : inboxAfter[v] # {})
                         \/ runningCount > 0
       IN
       /\ waitingReady' = IF drainFired THEN FALSE ELSE waitingReady
       /\ fiberPC' = pcAfter
       /\ inbox' = inboxAfter
       /\ IF drainFired
          THEN /\ wakeEpoch' = wakeEpoch + pubSignal
               /\ EraseIdleBumping(BumpPubErase)
          ELSE /\ UNCHANGED <<wakeEpoch, idleCount, contributionGen>>
       /\ IF mwS1After
          THEN workerStage' = [workerStage EXCEPT ![w] = "MwS1Idle"]
          ELSE workerStage' = [workerStage EXCEPT ![w] = "DanceGo"]
       /\ UNCHANGED <<held, writerActive, activeReaders, r2Queued,
                      r2Acquired, runningCount, observedEpoch, contributed,
                      contribGen, terminate, resCount, pubCount>>

(* scheduler.cpp:622 — the UNLOCKED mw_s1 fall-through idle reset between
   the unlocked classify (:567) and the dance-block entry, STEP 1 of 2 (the
   exchange(0)). Same split-window discipline as EraseIdleOnPop: the bump
   (:624) is its own arbitrarily-delayable action, owed exactly when this
   exchange actually erased a contribution. *)
EraseIdleMwS1(w) ==
    /\ workerStage[w] = "MwS1Idle"
    /\ idleCount' = 0
    /\ workerStage' = [workerStage EXCEPT ![w] =
                           IF RepairContributionGeneration /\ BumpMwS1Erase
                              /\ idleCount > 0
                           THEN "MwS1BumpPending"
                           ELSE "MwS1GRecheck"]
    /\ UNCHANGED <<held, inbox, fiberPC, writerActive, activeReaders,
                   r2Queued, r2Acquired, waitingReady, runningCount,
                   wakeEpoch, observedEpoch, contributed, contributionGen,
                   contribGen, terminate, resCount, pubCount>>

(* scheduler.cpp:624 — the mw_s1 fall-through generation bump, STEP 2 of 2.
   Sequenced strictly after its exchange; the eraser's next steps (the
   terminate load, the park commit) wait for it via the stage gate. *)
BumpMwS1Gen(w) ==
    /\ workerStage[w] = "MwS1BumpPending"
    /\ contributionGen' = contributionGen + 1
    /\ workerStage' = [workerStage EXCEPT ![w] = "MwS1GRecheck"]
    /\ UNCHANGED <<held, inbox, fiberPC, writerActive, activeReaders,
                   r2Queued, r2Acquired, waitingReady, runningCount,
                   wakeEpoch, observedEpoch, idleCount, contributed,
                   contribGen, terminate, resCount, pubCount>>

(* scheduler.cpp:942-958 — the dance-block reclassify under G. mw_s1 still:
   the :958 erase and fall through to the park candidate; otherwise the
   world became quiescent/MW-S3 and this worker MUST take the dance path
   (the straight-to-park shortcut here was the first counterexample's
   artifact — the C++ reclassify is the gate that prevents it). *)
ReclassifyMwS1(w) ==
    /\ workerStage[w] = "MwS1GRecheck"
    /\ IF TicketVisible \/ runningCount > 0
       THEN /\ EraseIdleBumping(BumpRecheckErase)
            /\ workerStage' = [workerStage EXCEPT ![w] = "ParkCandidate"]
       ELSE /\ UNCHANGED <<idleCount, contributionGen>>
            /\ workerStage' = [workerStage EXCEPT ![w] = "DanceGo"]
    /\ UNCHANGED <<held, inbox, fiberPC, writerActive, activeReaders,
                   r2Queued, r2Acquired, waitingReady, runningCount,
                   wakeEpoch, observedEpoch, idleCount, contributed,
                   contribGen, terminate, resCount, pubCount>>

(* =========================================================================
   The idle dance (scheduler.cpp:1035-1093, one G section). Contribute; if
   last (prev+1 >= live) re-check and TERMINATE unless work appeared (the
   :1065 reset-continue is folded here as the middle branch); if not last,
   signal (E9-LIFE-8) and fall through to the park candidate.
   ========================================================================= *)
DanceContribute(w) ==
    /\ workerStage[w] = "DanceGo"
    /\ LET prev == idleCount
           lastDancer == prev + 1 >= LiveWorkers
           recheckMwS1 == TicketVisible \/ runningCount > 0
       IN
       /\ IF lastDancer /\ ~recheckMwS1
          THEN /\ terminate' = TRUE
               /\ wakeEpoch' = wakeEpoch + 1
               /\ idleCount' = idleCount
               /\ contributionGen' = contributionGen
               /\ workerStage' = [workerStage EXCEPT ![w] = "Exited"]
               /\ contributed' = [contributed EXCEPT ![w] = 1]
               /\ contribGen' = [contribGen EXCEPT ![w] = contributionGen]
          ELSE IF lastDancer /\ recheckMwS1
          THEN (* :1065 reset-continue: work appeared between classify and
                  the dance; re-loop to run it. No signal (the work itself
                  is the observer's to run). Fidelity fix (review of the
                  split-window round): the C++ `continue` re-enters the
                  loop top, whose FIRST statement re-clears
                  idle_dance_contributed_ (:513) — a re-looping eraser
                  holds NO live contribution, so the model must not leave
                  contributed=1 at LoopTop (a stale 1 with a stale record
                  could fire the identity term on a ParkCandidate pass the
                  C++ would take with contributed=0 — a spurious refusal
                  that over-approximates the repair). *)
               /\ EraseIdleBumping(BumpDanceResetErase)
               /\ UNCHANGED <<terminate, wakeEpoch>>
               /\ workerStage' = [workerStage EXCEPT ![w] = "LoopTop"]
               /\ contributed' = [contributed EXCEPT ![w] = 0]
               /\ UNCHANGED contribGen
          ELSE (* not last: E9-LIFE-8 not-last signal *)
               /\ wakeEpoch' = wakeEpoch + 1
               /\ idleCount' = idleCount + 1
               /\ contributionGen' = contributionGen
               /\ UNCHANGED terminate
               /\ workerStage' = [workerStage EXCEPT ![w] = "ParkCandidate"]
               /\ contributed' = [contributed EXCEPT ![w] = 1]
               /\ contribGen' = [contribGen EXCEPT ![w] = contributionGen]
       /\ UNCHANGED <<held, inbox, fiberPC, writerActive, activeReaders,
                      r2Queued, r2Acquired, waitingReady, runningCount,
                      observedEpoch, resCount, pubCount>>

(* =========================================================================
   Park commit (park_wake.cpp:295-338, one G section: recheck then arm).
   Refusal signals the wake domain and re-loops (the contribution flag
   resets with the fresh iteration, C++ :509); commit arms the baseline on
   the CURRENT epoch — a signal that fired before the arm is absorbed by
   the baseline (the M4 mechanism).
   ========================================================================= *)
ParkCommit(w) ==
    /\ workerStage[w] = "ParkCandidate"
    /\ IF RefusePark(w)
       THEN /\ wakeEpoch' = wakeEpoch + 1
            /\ workerStage' = [workerStage EXCEPT ![w] = "LoopTop"]
            /\ contributed' = [contributed EXCEPT ![w] = 0]
            /\ UNCHANGED observedEpoch
       ELSE /\ observedEpoch' = [observedEpoch EXCEPT ![w] = wakeEpoch]
            /\ workerStage' = [workerStage EXCEPT ![w] = "Parked"]
            /\ UNCHANGED <<wakeEpoch, contributed>>
    /\ UNCHANGED <<held, inbox, fiberPC, writerActive, activeReaders,
                   r2Queued, r2Acquired, waitingReady, runningCount,
                   idleCount, contributionGen, contribGen,
                   terminate, resCount, pubCount>>

(* The cv predicate result: leave the park; terminate exits the loop. The
   woken worker re-enters the loop top (contribution reset, C++ :509). *)
LeavePark(w) ==
    /\ workerStage[w] = "Parked"
    /\ CanLeavePark(w)
    /\ workerStage' = [workerStage EXCEPT ![w] =
                           IF terminate THEN "Exited" ELSE "LoopTop"]
    /\ contributed' = [contributed EXCEPT ![w] = 0]
    /\ UNCHANGED <<held, inbox, fiberPC, writerActive, activeReaders,
                   r2Queued, r2Acquired, waitingReady, runningCount,
                   wakeEpoch, observedEpoch, idleCount, contributionGen,
                   contribGen, terminate, resCount, pubCount>>

(* scheduler.cpp:583/:1098 — the loop-path terminate breaks. *)
ExitOnTerminate(w) ==
    /\ terminate
    /\ workerStage[w] \in {"LoopTop", "MwS1Idle", "MwS1GRecheck",
                           "DanceGo", "ParkCandidate"}
    /\ workerStage' = [workerStage EXCEPT ![w] = "Exited"]
    /\ UNCHANGED <<held, inbox, fiberPC, writerActive, activeReaders,
                   r2Queued, r2Acquired, waitingReady, runningCount,
                   wakeEpoch, observedEpoch, idleCount, contributed,
                   contributionGen, contribGen, terminate, resCount,
                   pubCount>>

(* =========================================================================
   Fiber execution steps. WF's unlock is one action (C++ holds G across
   clear+claim+commit+publish+signal); the final step of each fiber folds
   the scheduler continuation (running--, :1236) and the loop-top
   contribution reset.
   ========================================================================= *)
FiberStepWF(w) ==
    /\ workerStage[w] = "Executing"
    /\ held[w] = WF
    /\ fiberPC[WF] = "RunUnlock"
    /\ writerActive' = FALSE
    /\ r2Queued' = FALSE
    /\ resCount' = [resCount EXCEPT ![R2] = 1]
    /\ activeReaders' = IF M5GrantNoTicket THEN activeReaders
                        ELSE activeReaders + 1
    /\ IF M5GrantNoTicket
       THEN /\ fiberPC' = [fiberPC EXCEPT ![WF] = "Done"]
            /\ UNCHANGED <<inbox, wakeEpoch, idleCount, contributionGen,
                           pubCount>>
       ELSE /\ fiberPC' = [fiberPC EXCEPT ![WF] = "Done", ![R2] = "Ticket"]
            /\ inbox' = [inbox EXCEPT ![W1] = inbox[W1] \cup {R2}]
            /\ pubCount' = [pubCount EXCEPT ![R2] = 1]
            /\ wakeEpoch' = wakeEpoch + (IF M1NoPubSignal THEN 0 ELSE 1)
            /\ EraseIdleBumping(BumpPubErase)
    /\ runningCount' = runningCount - 1
    /\ held' = [held EXCEPT ![w] = NOFIBER]
    /\ workerStage' = [workerStage EXCEPT ![w] = "LoopTop"]
    /\ contributed' = [contributed EXCEPT ![w] = 0]
    /\ UNCHANGED <<r2Acquired, waitingReady, observedEpoch,
                   contribGen, terminate>>

FiberStepR1(w) ==
    /\ workerStage[w] = "Executing"
    /\ held[w] = R1
    /\ fiberPC[R1] = "RunNoop"
    /\ fiberPC' = [fiberPC EXCEPT ![R1] = "Done"]
    /\ runningCount' = runningCount - 1
    /\ held' = [held EXCEPT ![w] = NOFIBER]
    /\ workerStage' = [workerStage EXCEPT ![w] = "LoopTop"]
    /\ contributed' = [contributed EXCEPT ![w] = 0]
    /\ UNCHANGED <<inbox, writerActive, activeReaders, r2Queued,
                   r2Acquired, waitingReady, wakeEpoch, observedEpoch,
                   idleCount, contributionGen, contribGen,
                   terminate, resCount, pubCount>>

FiberStepR2Flag(w) ==
    /\ workerStage[w] = "Executing"
    /\ held[w] = R2
    /\ fiberPC[R2] = "RunFlag"
    /\ r2Acquired' = TRUE
    /\ fiberPC' = [fiberPC EXCEPT ![R2] = "RunUnlockRead"]
    /\ UNCHANGED <<inbox, writerActive, activeReaders, r2Queued,
                   waitingReady, runningCount, wakeEpoch, observedEpoch,
                   idleCount, contributed, contributionGen, contribGen,
                   terminate, resCount, pubCount, held, workerStage>>

FiberStepR2Unlock(w) ==
    /\ workerStage[w] = "Executing"
    /\ held[w] = R2
    /\ fiberPC[R2] = "RunUnlockRead"
    /\ activeReaders' = 0
    /\ fiberPC' = [fiberPC EXCEPT ![R2] = "Done"]
    /\ runningCount' = runningCount - 1
    /\ held' = [held EXCEPT ![w] = NOFIBER]
    /\ workerStage' = [workerStage EXCEPT ![w] = "LoopTop"]
    /\ contributed' = [contributed EXCEPT ![w] = 0]
    /\ UNCHANGED <<inbox, writerActive, r2Queued, r2Acquired,
                   waitingReady, wakeEpoch, observedEpoch,
                   idleCount, contributionGen, contribGen,
                   terminate, resCount, pubCount>>

(* =========================================================================
   Next / Spec / fairness
   ========================================================================= *)
FiberStep(w) ==
    \/ FiberStepWF(w)
    \/ FiberStepR1(w)
    \/ FiberStepR2Flag(w)
    \/ FiberStepR2Unlock(w)

Next ==
    \/ \E w \in Workers :
        \/ TryPop(w)
        \/ TrySteal(w)
        \/ EraseIdleOnPop(w)
        \/ BumpPopGen(w)
        \/ StartFiber(w)
        \/ DrainClassify(w)
        \/ EraseIdleMwS1(w)
        \/ BumpMwS1Gen(w)
        \/ ReclassifyMwS1(w)
        \/ DanceContribute(w)
        \/ ParkCommit(w)
        \/ LeavePark(w)
        \/ ExitOnTerminate(w)
        \/ FiberStep(w)

Fairness ==
    /\ \A w \in Workers :
        \* SF (not WF) for the own-inbox pop: a steal-war between the two
        \* workers can toggle TryPop's enablement forever (each steal
        \* displaces the other's pop window), so WF never fires. SF encodes
        \* the minimal scheduling fact the C++ relies on: a worker whose
        \* own inbox holds a ticket and which is scheduled infinitely often
        \* eventually executes the straight-line pop. This is fairness on a
        \* scheduler-controlled action, not on any signal or producer.
        /\ SF_vars (TryPop(w))
        /\ WF_vars (TrySteal(w))
        /\ WF_vars (EraseIdleOnPop(w))
        /\ WF_vars (BumpPopGen(w))
        /\ WF_vars (StartFiber(w))
        /\ WF_vars (DrainClassify(w))
        /\ WF_vars (EraseIdleMwS1(w))
        /\ WF_vars (BumpMwS1Gen(w))
        /\ WF_vars (ReclassifyMwS1(w))
        /\ WF_vars (DanceContribute(w))
        /\ WF_vars (ParkCommit(w))
        /\ WF_vars (LeavePark(w))
        /\ WF_vars (ExitOnTerminate(w))
        /\ WF_vars (FiberStep(w))

Spec == Init /\ [][Next]_vars /\ Fairness

(* =========================================================================
   Safety invariants
   ========================================================================= *)
NoReaderWriterOverlap ==
    writerActive => activeReaders = 0

TerminalUniqueness ==
    \A f \in Fibers : resCount[f] <= 1

PublicationUniqueness ==
    \A f \in Fibers : pubCount[f] <= 1

(* The rwlock queue holds only unresolved waiters. *)
NoLinkedTerminal ==
    r2Queued => fiberPC[R2] = "QueueWait"

(* Structural sanity: an inbox ticket is a Ticket-state fiber; a held
   ticket is a Ticket-state fiber in the pop window or a Run-state fiber
   while Executing. *)
InboxImpliesTicket ==
    (\A v \in Workers : \A f \in inbox[v] : fiberPC[f] = "Ticket")
    /\ (\A w \in Workers :
        (held[w] # NOFIBER =>
            fiberPC[held[w]] \in {"Ticket", "RunUnlock", "RunNoop",
                                  "RunFlag", "RunUnlockRead"}))

(* #115-class guard: a visible runnable ticket must never coexist with all
   workers parked (no observer) and no termination in flight. *)
NoStrandedRunnable ==
    ~(TicketVisible /\ terminate = FALSE
      /\ \A w \in Workers : workerStage[w] = "Parked")

(* The #161 target bad state: every worker asleep in the unbounded park,
   the dance one short of convergence, no work, no waits, no terminate, and
   NO park leave enabled (every baseline already absorbed every signal) —
   only stuttering remains; run_impl's join hangs forever. The no-leave
   conjunct matters: a freshly-signaled parked worker (baseline predates
   the signal) is a transient, converging state, not a stuck one. *)
DrainStuckState ==
    ~(   \A w \in Workers : workerStage[w] = "Parked"
      /\ idleCount < LiveWorkers
      /\ terminate = FALSE
      /\ ~TicketVisible
      /\ runningCount = 0
      /\ waitingReady = FALSE
      /\ \A u \in Workers : ~CanLeavePark(u))

(* Split-window reachability witness (REPAIRED constants only — negative
   gate E12SchedLivenessSplitWindow expects this VIOLATED): a park commit
   can read the erased count together with the STILL-CURRENT generation —
   the eraser's exchange(0) landed but its generation bump has not — and so
   arm a baseline WITHOUT the identity-term refusal. The state is
   reachable only through the split: once w holds contributed=1, every
   count-zeroing path except a pending split bump advances the generation
   past w's record (the atomic G-section erases bump when they erase), so
   idleCount = 0 /\ contributionGen = contribGen[w] pins the moment
   between the eraser's two unlocked RMWs. The witness proves the model
   genuinely explores the C++ split window; the POSITIVE safety gate
   (DrainStuckState, same constants) proves what the window costs — only a
   transient park: the eraser's bump, run, and re-dance not-last signal are
   all sequenced after the arming, the cv predicate fires, and the dancer
   re-loops instead of sleeping through an absorbed signal. *)
SplitWindowNeverArmed ==
    ~\E w \in Workers :
        /\ workerStage[w] = "Parked"
        /\ contributed[w] = 1
        /\ idleCount = 0
        /\ contributionGen = contribGen[w]

(* =========================================================================
   Liveness properties (~> / <>; fairness only on the WF'd actions above)
   ========================================================================= *)
RunReturned ==
    terminate /\ \A w \in Workers : workerStage[w] = "Exited"

Complete ==
    r2Acquired /\ RunReturned

(* P1: the T22 scenario completes — R2 granted and run, run returned. *)
CancelUnlockScenarioEventuallyCompletes == <>Complete

(* P2: a Drain run always returns (the join never hangs). *)
DrainEventuallyReturns == <>RunReturned

(* P3: a granted waiter's ticket is eventually executed. *)
GrantedWaiterEventuallyResumes ==
    [](fiberPC[R2] = "Ticket" => <>(fiberPC[R2] = "Done"))

(* P4: a visible runnable ticket eventually starts executing. *)
RunnableEventuallyExecutes ==
    [](TicketVisible => <>(runningCount > 0))

=============================================================================
