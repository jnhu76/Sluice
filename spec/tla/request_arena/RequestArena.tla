------------------- MODULE RequestArena -------------------
(***************************************************************************)
(* sluice formal suite: request-arena                                       *)
(*                                                                          *)
(* Protocol: RequestArena / RequestSlot explicit-I/O accepted-request       *)
(* lifecycle — the load-bearing slot protocol behind every migrated async   *)
(* backend (ADR-explicit-io-request-contract, Accepted).                    *)
(*                                                                          *)
(* This suite closes the manifest's former `request-arena-lifecycle`        *)
(* ACCEPTED FORMAL-DEBT gap with the smallest model that captures the       *)
(* load-bearing races (AGENTS.md section 17):                               *)
(*                                                                          *)
(*   - five-stage admission: reserve -> prepare -> (install binding)        *)
(*     -> commit -> enqueue;                                                *)
(*   - Scheme-B enqueue/cancel arbitration: a pending cancel may win the    *)
(*     terminal directly; a later enqueue observes backend_ready and is a   *)
(*     successful no-op that acknowledges the enqueue pin;                  *)
(*   - terminal-winner exactly-once (first valid write wins, losers never   *)
(*     overwrite);                                                          *)
(*   - running-cancel records INTENT only; the ordinary syscall result is   *)
(*     recorded VERBATIM (never rewritten to canceled) unless the backend   *)
(*     CONFIRMS the interruption (ADR Decision 11);                         *)
(*   - reap is the ONLY Completion-ready publication path, and only after   *)
(*     the enqueue pin is acknowledged (I17/I19);                           *)
(*   - generation increments before slot reuse; a stale key never mutates   *)
(*     a later occupant;                                                    *)
(*   - borrow runs commit -> completion-ready publication (I7/I18);         *)
(*   - quiescent destruction requires an idle arena.                       *)
(*                                                                          *)
(* C++ authority (single arena leaf mutex = atomic actions here; TLA        *)
(* interleaving models mutex acquisition order):                            *)
(*   include/sluice/async/detail/request_arena.hpp                          *)
(*   include/sluice/async/detail/request_slot.hpp                           *)
(*                                                                          *)
(* Authority layering (PR #125 review P1): this model separates two         *)
(* authorities and must never blur them.                                    *)
(*   Layer A — leaf safety (modeled, proven here): everything the arena     *)
(*     leaf itself enforces under its one mutex — admission staging,        *)
(*     Scheme-B arbitration via the arena's own cancel() entry, terminal    *)
(*     exactly-once, generation advance, pin/reap gating, borrow window.    *)
(*   Layer B — external obligations (ASSUMED here, owned by callers):       *)
(*     - Progress: WF(Enqueue)/WF(Reap) below are obligations of the        *)
(*       backend submit path and the backend/runtime progress loop          *)
(*       (src/async/threadpool_backend.cpp enqueue_after_commit / poll /    *)
(*       wait_one; src/async/uring_backend.cpp reaper paths). The arena     *)
(*       itself cannot make anyone invoke enqueue or reap.                  *)
(*     - Decision-11 provenance: RecordCanceledConfirmed models the BACKEND *)
(*       obligation to call record_canceled only after a CONFIRMED          *)
(*       interruption. The C++ leaf record_canceled()/record_terminal()     *)
(*       checks only slot state and exactly-once — no cancel-intent or      *)
(*       provenance check — and no production backend currently calls it    *)
(*       (tests simulate the confirming backend; NEG-RA-6 pins the          *)
(*       ill-behaved caller).                                               *)
(*                                                                          *)
(* Scope decisions (see README.md for the full mapping table):              *)
(*   - ONE slot (capacity 1). The would_block admission path is exercised   *)
(*     structurally (a busy slot admits nothing); multi-slot               *)
(*     interference and ready-ring FIFO ORDER across slots remain          *)
(*     executable-evidence scope (request_arena_test / scheme-b tests).    *)
(*   - MaxGen bounds the state space only; it is not a protocol bound.     *)
(*   - The backend admission transaction (Completion idle->binding->        *)
(*     outstanding publication around commit) is a different protocol       *)
(*     (Completion authority, AGENTS.md 4.3) and is NOT modeled here.      *)
(*                                                                          *)
(* Negative models use the e13 FAULT-constant pattern: each Fault_* action  *)
(* is enabled only under its FAULT value and performs exactly the buggy     *)
(* transition; each fault cfg asserts ONLY the named target invariant, so  *)
(* the verifier's named check is exact. FAULT = "None" (the positive cfg)  *)
(* doubles as the restore gate.                                             *)
(***************************************************************************)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS MaxGen, FAULT

FaultActive(name) == FAULT = name

Gen == 0..MaxGen
AcceptedStates == {"pending", "enqueued", "running", "backend_ready"}
SlotStates == {"free", "reserved", "prepared"} \union AcceptedStates
             \union {"completion_ready"}
RegStates == {"open_no_waiter", "open_registered", "closed"}
\* Who wrote a canceled terminal. "cancel_running" is UNREACHABLE in the
\* correct protocol (running cancel records intent only — Decision 11).
CancelSources == {"none", "record_terminal", "cancel_pending",
                  "cancel_enqueued", "cancel_running", "confirmed_interruption"}
LegitimateCancelSources == {"cancel_pending", "cancel_enqueued",
                            "confirmed_interruption"}

VARIABLES state, gen, pin, terminalStored, terminalCanceled, cancelIntent,
          regState, deliveryPresent, borrowActive, bindingInstalled,
          slotInUse, acceptedOutstanding, onRing, admissionClosed, destroyed,
          committed, terminalWinner, published, freed, cancelSource,
          intentSeen, waiterDelivered

vars == <<state, gen, pin, terminalStored, terminalCanceled, cancelIntent,
          regState, deliveryPresent, borrowActive, bindingInstalled,
          slotInUse, acceptedOutstanding, onRing, admissionClosed, destroyed,
          committed, terminalWinner, published, freed, cancelSource,
          intentSeen, waiterDelivered>>

(***************************************************************************)
(* Correct protocol actions.                                               *)
(* Every mutating C++ arena method takes the one leaf slot-lifecycle mutex;*)
(* each action below is one such critical section.                         *)
(***************************************************************************)

\* Stage 1: reserve pops the free list and stamps key generation = slot gen.
\* gen < MaxGen is a state-space cap for TLC, not a protocol rule; within it,
\* at least one release/reuse cycle is fully explored.
Reserve ==
  /\ state = "free"
  /\ gen < MaxGen
  /\ ~admissionClosed
  /\ ~destroyed
  /\ state' = "reserved"
  /\ pin' = FALSE
  /\ terminalStored' = FALSE
  /\ terminalCanceled' = FALSE
  /\ cancelIntent' = FALSE
  /\ regState' = "open_no_waiter"
  /\ deliveryPresent' = FALSE
  /\ borrowActive' = FALSE
  /\ bindingInstalled' = FALSE
  /\ onRing' = FALSE
  /\ slotInUse' = 1
  /\ UNCHANGED <<gen, acceptedOutstanding, admissionClosed, destroyed,
                  committed, terminalWinner, published, freed, cancelSource,
                  intentSeen, waiterDelivered>>

\* Stage 2: prepare writes kind/borrow metadata. Borrow does NOT begin here.
Prepare ==
  /\ state = "reserved"
  /\ state' = "prepared"
  /\ UNCHANGED <<gen, pin, terminalStored, terminalCanceled, cancelIntent,
                  regState, deliveryPresent, borrowActive, bindingInstalled,
                  slotInUse, acceptedOutstanding, onRing, admissionClosed,
                  destroyed, committed, terminalWinner, published, freed,
                  cancelSource, intentSeen, waiterDelivered>>

\* Stage 2.5: install the Completion publication binding (review C2). One
\* binding per slot generation; validated by reap before any accounting.
InstallBinding ==
  /\ state = "prepared"
  /\ ~bindingInstalled
  /\ bindingInstalled' = TRUE
  /\ UNCHANGED <<state, gen, pin, terminalStored, terminalCanceled,
                  cancelIntent, regState, deliveryPresent, borrowActive,
                  slotInUse, acceptedOutstanding, onRing, admissionClosed,
                  destroyed, committed, terminalWinner, published, freed,
                  cancelSource, intentSeen, waiterDelivered>>

\* Stage 3: commit/accept. Pin set BEFORE outstanding publication; borrow
\* begins; accepted_outstanding++. No admission re-check (a reserved slot is
\* an in-flight submission that must complete — ADR Decision 15 note).
Commit ==
  /\ state = "prepared"
  /\ bindingInstalled
  /\ state' = "pending"
  /\ pin' = TRUE
  /\ borrowActive' = TRUE
  /\ acceptedOutstanding' = 1
  /\ committed' = [committed EXCEPT ![gen] = TRUE]
  /\ UNCHANGED <<gen, terminalStored, terminalCanceled, cancelIntent,
                  regState, deliveryPresent, bindingInstalled, slotInUse,
                  onRing, admissionClosed, destroyed, terminalWinner,
                  published, freed, cancelSource, intentSeen, waiterDelivered>>

\* Pre-commit rollback authority: returns a never-accepted slot. Generation
\* still increments before reuse (the stale key of the rolled-back occupant
\* must never match a later occupant).
RollbackPreCommit ==
  /\ state \in {"reserved", "prepared"}
  /\ state' = "free"
  /\ gen' = gen + 1
  /\ slotInUse' = 0
  /\ bindingInstalled' = FALSE
  /\ freed' = [freed EXCEPT ![gen] = TRUE]
  /\ UNCHANGED <<pin, terminalStored, terminalCanceled, cancelIntent,
                  regState, deliveryPresent, borrowActive, acceptedOutstanding,
                  onRing, admissionClosed, destroyed, committed,
                  terminalWinner, published, cancelSource, intentSeen,
                  waiterDelivered>>

\* Stage 4: enqueue. Allocation-free final submit-path slot access.
\*   pending        -> enqueued            (dispatchable)
\*   backend_ready  -> unchanged (Scheme-B terminal_noop: a pending cancel or
\*                     other terminal winner got there first)
\* Either way the enqueue pin is acknowledged (cleared).
Enqueue ==
  /\ pin
  /\ \/ /\ state = "pending"
        /\ state' = "enqueued"
     \/ /\ state = "backend_ready"
        /\ state' = state
  /\ pin' = FALSE
  /\ UNCHANGED <<gen, terminalStored, terminalCanceled, cancelIntent,
                  regState, deliveryPresent, borrowActive, bindingInstalled,
                  slotInUse, acceptedOutstanding, onRing, admissionClosed,
                  destroyed, committed, terminalWinner, published, freed,
                  cancelSource, intentSeen, waiterDelivered>>

\* Dispatch ownership: enqueued -> running before the syscall blocks.
MarkRunning ==
  /\ state = "enqueued"
  /\ state' = "running"
  /\ UNCHANGED <<gen, pin, terminalStored, terminalCanceled, cancelIntent,
                  regState, deliveryPresent, borrowActive, bindingInstalled,
                  slotInUse, acceptedOutstanding, onRing, admissionClosed,
                  destroyed, committed, terminalWinner, published, freed,
                  cancelSource, intentSeen, waiterDelivered>>

\* Terminal winner: the ordinary syscall result, VERBATIM (success or error —
\* both are "ordinary" here). Consumes any live cancel intent.
RecordTerminal ==
  /\ ~terminalStored
  /\ state \in {"pending", "enqueued", "running"}
  /\ terminalStored' = TRUE
  /\ terminalCanceled' = FALSE
  /\ cancelIntent' = FALSE
  /\ state' = "backend_ready"
  /\ onRing' = TRUE
  /\ terminalWinner' = [terminalWinner EXCEPT ![gen] = @ + 1]
  /\ cancelSource' = [cancelSource EXCEPT ![gen] = "record_terminal"]
  /\ UNCHANGED <<gen, pin, regState, deliveryPresent, borrowActive,
                  bindingInstalled, slotInUse, acceptedOutstanding,
                  admissionClosed, destroyed, committed, published, freed,
                  intentSeen, waiterDelivered>>

\* ENVIRONMENT/BACKEND OBLIGATION (Layer B, PR #125 review P1-2) — NOT a
\* leaf-enforced capability. A backend that CONFIRMS a running interruption
\* took effect may store the canceled terminal explicitly (ADR Decision 11)
\* — this is the only running path to a canceled terminal. The guards below
\* (running + live cancelIntent + source stamp "confirmed_interruption")
\* model the WELL-BEHAVED caller the obligation permits. The C++ leaf
\* record_canceled(h) is only record_terminal(err(canceled)): it validates
\* handle generation and slot state and exactly-once, but does NOT check
\* cancel_intent_ or any interruption provenance — any current-generation
\* caller on an accepted slot could write it. No production backend calls
\* record_canceled today; tests simulate the confirming backend, and
\* FaultRunningCancelStores (NEG-RA-6) demonstrates the ill-behaved caller
\* this invariant catches. Consequently InvCanceledTerminalSource is an
\* environment-contract invariant: it proves "IF callers honor the
\* obligation THEN no intent-only running cancel produces a canceled
\* terminal" — it does NOT prove the leaf enforces the discipline.
RecordCanceledConfirmed ==
  /\ ~terminalStored
  /\ state = "running"
  /\ cancelIntent
  /\ terminalStored' = TRUE
  /\ terminalCanceled' = TRUE
  /\ cancelIntent' = FALSE
  /\ state' = "backend_ready"
  /\ onRing' = TRUE
  /\ terminalWinner' = [terminalWinner EXCEPT ![gen] = @ + 1]
  /\ cancelSource' = [cancelSource EXCEPT ![gen] = "confirmed_interruption"]
  /\ UNCHANGED <<gen, pin, regState, deliveryPresent, borrowActive,
                  bindingInstalled, slotInUse, acceptedOutstanding,
                  admissionClosed, destroyed, committed, published, freed,
                  intentSeen, waiterDelivered>>

\* Scheme-B cancel on pending/enqueued: cancel WINS the terminal directly.
\* On pending the enqueue pin STAYS LIVE — reap cannot publish until the
\* submit path's enqueue no-ops and acknowledges it (I17/I19).
CancelPendingOrEnqueued ==
  /\ ~terminalStored
  /\ state \in {"pending", "enqueued"}
  /\ terminalStored' = TRUE
  /\ terminalCanceled' = TRUE
  /\ cancelIntent' = FALSE
  /\ state' = "backend_ready"
  /\ onRing' = TRUE
  /\ terminalWinner' = [terminalWinner EXCEPT ![gen] = @ + 1]
  /\ cancelSource' = [cancelSource EXCEPT ![gen] =
                        IF state = "pending" THEN "cancel_pending"
                        ELSE "cancel_enqueued"]
  /\ UNCHANGED <<gen, pin, regState, deliveryPresent, borrowActive,
                  bindingInstalled, slotInUse, acceptedOutstanding,
                  admissionClosed, destroyed, committed, published, freed,
                  intentSeen, waiterDelivered>>

\* Running cancel records INTENT only (Decision 11): no terminal, no ring
\* push. The ordinary result later competes via RecordTerminal verbatim.
CancelRunningIntent ==
  /\ ~terminalStored
  /\ state = "running"
  /\ ~cancelIntent
  /\ cancelIntent' = TRUE
  /\ intentSeen' = [intentSeen EXCEPT ![gen] = TRUE]
  /\ UNCHANGED <<state, gen, pin, terminalStored, terminalCanceled, regState,
                  deliveryPresent, borrowActive, bindingInstalled, slotInUse,
                  acceptedOutstanding, onRing, admissionClosed, destroyed,
                  committed, terminalWinner, published, freed, cancelSource,
                  waiterDelivered>>

\* Single-waiter registration: legal on any accepted, unreaped request —
\* INCLUDING backend_ready (register-after-terminal-before-reap is legal;
\* reap delivers). Only reap closes registration.
RegisterWaiter ==
  /\ state \in AcceptedStates
  /\ regState = "open_no_waiter"
  /\ regState' = "open_registered"
  /\ deliveryPresent' = TRUE
  /\ UNCHANGED <<state, gen, pin, terminalStored, terminalCanceled,
                  cancelIntent, borrowActive, bindingInstalled, slotInUse,
                  acceptedOutstanding, onRing, admissionClosed, destroyed,
                  committed, terminalWinner, published, freed, cancelSource,
                  intentSeen, waiterDelivered>>

\* Wait-cancel races reap exactly-once for the delivery.
CancelWaiter ==
  /\ regState = "open_registered"
  /\ regState' = "open_no_waiter"
  /\ deliveryPresent' = FALSE
  /\ UNCHANGED <<state, gen, pin, terminalStored, terminalCanceled,
                  cancelIntent, borrowActive, bindingInstalled, slotInUse,
                  acceptedOutstanding, onRing, admissionClosed, destroyed,
                  committed, terminalWinner, published, freed, cancelSource,
                  intentSeen, waiterDelivered>>

\* Stage 5: reap — the ONLY Completion-ready publication. Requires the
\* acknowledged pin (a pinned backend_ready head stays reap-ineligible and
\* remains ring head — level-triggered), a stored terminal, and the
\* publication binding. One atomic leaf-domain step closes registration
\* (taking any waiter delivery exactly-once), ends the borrow, decrements
\* accepted_outstanding, and publishes.
Reap ==
  /\ state = "backend_ready"
  /\ onRing
  /\ ~pin
  /\ terminalStored
  /\ bindingInstalled
  /\ state' = "completion_ready"
  /\ onRing' = FALSE
  /\ regState' = "closed"
  /\ waiterDelivered' = [waiterDelivered EXCEPT ![gen] = deliveryPresent]
  /\ deliveryPresent' = FALSE
  /\ borrowActive' = FALSE
  /\ acceptedOutstanding' = 0
  /\ published' = [published EXCEPT ![gen] = @ + 1]
  /\ UNCHANGED <<gen, pin, terminalStored, terminalCanceled, cancelIntent,
                  bindingInstalled, slotInUse, admissionClosed, destroyed,
                  committed, terminalWinner, freed, cancelSource, intentSeen>>

\* Caller handshake: Completion::reset() / destruction of a ready Completion
\* releases the slot. Requires post-reap state, acknowledged pin, closed
\* registration. Generation increments BEFORE the slot becomes visible to a
\* new reserve (I6).
ReleaseCompleted ==
  /\ state = "completion_ready"
  /\ ~pin
  /\ regState = "closed"
  /\ state' = "free"
  /\ gen' = gen + 1
  /\ slotInUse' = 0
  /\ terminalStored' = FALSE
  /\ terminalCanceled' = FALSE
  /\ cancelIntent' = FALSE
  /\ bindingInstalled' = FALSE
  /\ borrowActive' = FALSE
  /\ onRing' = FALSE
  /\ freed' = [freed EXCEPT ![gen] = TRUE]
  /\ UNCHANGED <<pin, regState, deliveryPresent, acceptedOutstanding,
                  admissionClosed, destroyed, committed, terminalWinner,
                  published, cancelSource, intentSeen, waiterDelivered>>

\* close_admission gates NEW acceptance at reserve; accepted requests run to
\* their ordinary terminal (ADR Decision 15). Guarded by ~destroyed (PR #125
\* review P2): a destroyed C++ object accepts no operations, so Destroy must
\* be TERMINAL — post-destruction only stuttering is legal, never a further
\* close_admission (formerly a TOO-BROAD spurious behavior).
CloseAdmission ==
  /\ ~admissionClosed
  /\ ~destroyed
  /\ admissionClosed' = TRUE
  /\ UNCHANGED <<state, gen, pin, terminalStored, terminalCanceled,
                  cancelIntent, regState, deliveryPresent, borrowActive,
                  bindingInstalled, slotInUse, acceptedOutstanding, onRing,
                  destroyed, committed, terminalWinner, published, freed,
                  cancelSource, intentSeen, waiterDelivered>>

\* Quiescent destruction: every slot free (AC-13 / ADR Decision 15). The C++
\* destructor fail-fasts otherwise; the model keeps destruction quiescent and
\* the fault-free invariants make busy destruction unrepresentable. Destroy
\* is TERMINAL: with Reserve and CloseAdmission both guarded by ~destroyed,
\* no action is enabled afterwards (every other action needs a non-free
\* occupant or live protocol state), so destroyed ⇒ only stuttering — the
\* behaviors of a C++ object that no longer exists.
Destroy ==
  /\ ~destroyed
  /\ state = "free"
  /\ slotInUse = 0
  /\ acceptedOutstanding = 0
  /\ destroyed' = TRUE
  /\ UNCHANGED <<state, gen, pin, terminalStored, terminalCanceled,
                  cancelIntent, regState, deliveryPresent, borrowActive,
                  bindingInstalled, slotInUse, acceptedOutstanding, onRing,
                  admissionClosed, committed, terminalWinner, published,
                  freed, cancelSource, intentSeen, waiterDelivered>>

(***************************************************************************)
(* Adversarial fault actions (e13 FAULT pattern). Each performs exactly the *)
(* buggy transition of a real implementable regression; enabled only under  *)
(* its FAULT value so the positive cfg (FAULT = "None") is the restore gate.*)
(***************************************************************************)

\* F1: record_terminal ignores the exactly-once check — a second winner
\* overwrites/double-pushes (target: InvNoDoubleTerminal). The
\* terminalWinner[gen] < 2 bound keeps the fault a SINGLE unforced write so
\* the state space stays finite (the named violation is 1 -> 2).
FaultDoubleTerminal ==
  /\ FaultActive("DoubleTerminal")
  /\ terminalStored
  /\ state = "backend_ready"
  /\ terminalWinner[gen] < 2
  /\ terminalWinner' = [terminalWinner EXCEPT ![gen] = @ + 1]
  /\ UNCHANGED <<state, gen, pin, terminalStored, terminalCanceled,
                  cancelIntent, regState, deliveryPresent, borrowActive,
                  bindingInstalled, slotInUse, acceptedOutstanding, onRing,
                  admissionClosed, destroyed, committed, published, freed,
                  cancelSource, intentSeen, waiterDelivered>>

\* F2: cancel resolves the slot but ignores the generation — a stale key
\* terminalizes the CURRENT occupant (AGENTS 10.1). The stale key must be
\* CAUSAL (PR #125 review P2): sg was actually issued to a caller
\* (committed[sg]) and its occupant already released (freed[sg]) — under the
\* pre-fault invariants freed[sg] implies sg < gen, so the current occupant
\* is precisely the W4 reuse chain: issued -> released -> generation reused
\* -> old key arrives -> buggy validation accepts it against the new
\* occupant. A fabricated sg that was never a real key would be a
\* ceremonial mutant. Fires on a reserved/prepared occupant (a not-yet-
\* accepted new occupant) to violate the accepted-terminal requirement;
\* firing on pending/enqueued/running would be indistinguishable from the
\* legitimate CancelPendingOrEnqueued outcome.
\* Scope note (review nitpick): the rogue write ALSO breaks InvAccounting
\* and InvBorrowWindow — a never-accepted occupant is promoted to an
\* accepted-state representation with no accepted_outstanding accounting
\* and no active borrow. That breakage is inherent to the modeled rogue
\* write (the C++ leaf fail-fasts record_terminal on reserved/prepared for
\* exactly this reason), not a separate fault; the cfg's named check stays
\* InvTerminalRequiresAccepted, the stale-identity law.
FaultStaleCancel ==
  /\ FaultActive("StaleCancel")
  /\ \E sg \in Gen : /\ committed[sg]
                      /\ freed[sg]
  /\ ~terminalStored
  /\ state \in {"reserved", "prepared"}
  /\ terminalStored' = TRUE
  /\ terminalCanceled' = TRUE
  /\ state' = "backend_ready"
  /\ onRing' = TRUE
  /\ terminalWinner' = [terminalWinner EXCEPT ![gen] = @ + 1]
  /\ cancelSource' = [cancelSource EXCEPT ![gen] = "cancel_pending"]
  /\ UNCHANGED <<gen, pin, cancelIntent, regState, deliveryPresent,
                  borrowActive, bindingInstalled, slotInUse,
                  acceptedOutstanding, admissionClosed, destroyed, committed,
                  published, freed, intentSeen, waiterDelivered>>

\* F3: a worker/CQE path publishes Completion-ready directly, bypassing reap
\* authority (AGENTS 4.2 / 10.6) — the publication is not coupled to the
\* close-registration/borrow-end/accounting handshake. published[gen] < 2
\* bounds the fault to one rogue publication.
FaultDirectPublish ==
  /\ FaultActive("DirectPublish")
  /\ state = "backend_ready"
  /\ onRing
  /\ published[gen] < 2
  /\ published' = [published EXCEPT ![gen] = @ + 1]
  /\ UNCHANGED <<state, gen, pin, terminalStored, terminalCanceled,
                  cancelIntent, regState, deliveryPresent, borrowActive,
                  bindingInstalled, slotInUse, acceptedOutstanding, onRing,
                  admissionClosed, destroyed, committed, terminalWinner,
                  freed, cancelSource, intentSeen, waiterDelivered>>

\* F4: release skips the generation increment (I6 violation) — the stale key
\* of the released occupant now matches the next occupant. The ~freed[gen]
\* bound keeps the fault a single occurrence (otherwise gen stays stuck and
\* winner counts grow without bound).
FaultNoGenIncrement ==
  /\ FaultActive("NoGenIncrement")
  /\ state = "completion_ready"
  /\ ~pin
  /\ regState = "closed"
  /\ ~freed[gen]
  /\ state' = "free"
  /\ slotInUse' = 0
  /\ terminalStored' = FALSE
  /\ terminalCanceled' = FALSE
  /\ cancelIntent' = FALSE
  /\ bindingInstalled' = FALSE
  /\ borrowActive' = FALSE
  /\ onRing' = FALSE
  /\ freed' = [freed EXCEPT ![gen] = TRUE]
  /\ UNCHANGED <<gen, pin, regState, deliveryPresent, acceptedOutstanding,
                  admissionClosed, destroyed, committed, terminalWinner,
                  published, cancelSource, intentSeen, waiterDelivered>>

\* F5: reap ignores the live enqueue pin and publishes a pinned backend_ready
\* head (I17/I19 violation — the submit path may still observe the slot).
FaultReapIgnoresPin ==
  /\ FaultActive("ReapIgnoresPin")
  /\ state = "backend_ready"
  /\ onRing
  /\ pin
  /\ terminalStored
  /\ bindingInstalled
  /\ state' = "completion_ready"
  /\ onRing' = FALSE
  /\ regState' = "closed"
  /\ waiterDelivered' = [waiterDelivered EXCEPT ![gen] = deliveryPresent]
  /\ deliveryPresent' = FALSE
  /\ borrowActive' = FALSE
  /\ acceptedOutstanding' = 0
  /\ published' = [published EXCEPT ![gen] = @ + 1]
  /\ UNCHANGED <<gen, pin, terminalStored, terminalCanceled, cancelIntent,
                  bindingInstalled, slotInUse, admissionClosed, destroyed,
                  committed, terminalWinner, freed, cancelSource, intentSeen>>

\* F6: cancel on a RUNNING slot stores the canceled terminal directly instead
\* of recording intent (Decision 11 violation — an ordinary syscall result is
\* silently rewritten to canceled).
FaultRunningCancelStores ==
  /\ FaultActive("RunningCancelStores")
  /\ ~terminalStored
  /\ state = "running"
  /\ terminalStored' = TRUE
  /\ terminalCanceled' = TRUE
  /\ cancelIntent' = FALSE
  /\ state' = "backend_ready"
  /\ onRing' = TRUE
  /\ terminalWinner' = [terminalWinner EXCEPT ![gen] = @ + 1]
  /\ cancelSource' = [cancelSource EXCEPT ![gen] = "cancel_running"]
  /\ UNCHANGED <<gen, pin, regState, deliveryPresent, borrowActive,
                  bindingInstalled, slotInUse, acceptedOutstanding,
                  admissionClosed, destroyed, committed, published, freed,
                  intentSeen, waiterDelivered>>

Init ==
  /\ state = "free"
  /\ gen = 0
  /\ pin = FALSE
  /\ terminalStored = FALSE
  /\ terminalCanceled = FALSE
  /\ cancelIntent = FALSE
  /\ regState = "open_no_waiter"
  /\ deliveryPresent = FALSE
  /\ borrowActive = FALSE
  /\ bindingInstalled = FALSE
  /\ slotInUse = 0
  /\ acceptedOutstanding = 0
  /\ onRing = FALSE
  /\ admissionClosed = FALSE
  /\ destroyed = FALSE
  /\ committed = [g \in Gen |-> FALSE]
  /\ terminalWinner = [g \in Gen |-> 0]
  /\ published = [g \in Gen |-> 0]
  /\ freed = [g \in Gen |-> FALSE]
  /\ cancelSource = [g \in Gen |-> "none"]
  /\ intentSeen = [g \in Gen |-> FALSE]
  /\ waiterDelivered = [g \in Gen |-> FALSE]

Next ==
  \/ Reserve
  \/ Prepare
  \/ InstallBinding
  \/ Commit
  \/ RollbackPreCommit
  \/ Enqueue
  \/ MarkRunning
  \/ RecordTerminal
  \/ RecordCanceledConfirmed
  \/ CancelPendingOrEnqueued
  \/ CancelRunningIntent
  \/ RegisterWaiter
  \/ CancelWaiter
  \/ Reap
  \/ ReleaseCompleted
  \/ CloseAdmission
  \/ Destroy
  \/ FaultDoubleTerminal
  \/ FaultStaleCancel
  \/ FaultDirectPublish
  \/ FaultNoGenIncrement
  \/ FaultReapIgnoresPin
  \/ FaultRunningCancelStores

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Invariants.                                                              *)
(***************************************************************************)

TypeOK ==
  /\ state \in SlotStates
  /\ gen \in Gen
  /\ pin \in BOOLEAN
  /\ terminalStored \in BOOLEAN
  /\ terminalCanceled \in BOOLEAN
  /\ cancelIntent \in BOOLEAN
  /\ regState \in RegStates
  /\ deliveryPresent \in BOOLEAN
  /\ borrowActive \in BOOLEAN
  /\ bindingInstalled \in BOOLEAN
  /\ slotInUse \in {0, 1}
  /\ acceptedOutstanding \in {0, 1}
  /\ onRing \in BOOLEAN
  /\ admissionClosed \in BOOLEAN
  /\ destroyed \in BOOLEAN
  /\ committed \in [Gen -> BOOLEAN]
  /\ terminalWinner \in [Gen -> {0, 1, 2}]
  /\ published \in [Gen -> {0, 1, 2}]
  /\ freed \in [Gen -> BOOLEAN]
  /\ cancelSource \in [Gen -> CancelSources]
  /\ intentSeen \in [Gen -> BOOLEAN]
  /\ waiterDelivered \in [Gen -> BOOLEAN]

\* The two DISTINCT counters (P1-05): slot_in_use spans reserve->release;
\* accepted_outstanding spans commit->reap-publication.
InvAccounting ==
  /\ slotInUse = IF state = "free" THEN 0 ELSE 1
  /\ acceptedOutstanding = IF state \in AcceptedStates THEN 1 ELSE 0

\* fd/buffer borrow runs commit -> completion-ready publication (I7/I18).
InvBorrowWindow ==
  /\ borrowActive <=> state \in AcceptedStates

\* The current occupant's terminal bit and its per-generation winner count
\* agree; a stored terminal only ever lives on a terminal state.
InvTerminalStates ==
  /\ terminalStored <=> (terminalWinner[gen] = 1)
  /\ terminalStored => state \in {"backend_ready", "completion_ready"}

\* Ready-ring membership is exactly the backend_ready state.
InvOnRingMatches == onRing <=> (state = "backend_ready")

\* The enqueue pin lives only between commit and the submit path's final
\* acknowledgement, on pending or a Scheme-B canceled slot.
InvPinPhase == pin => (committed[gen] /\ state \in {"pending", "backend_ready"})

\* Terminal-winner exactly-once, per occupant generation.
InvNoDoubleTerminal == \A g \in Gen : terminalWinner[g] <= 1

\* Completion-ready publication exactly-once, per occupant generation.
InvNoDoublePublication == \A g \in Gen : published[g] <= 1

\* Only an ACCEPTED request can hold a terminal (stale-key witness).
InvTerminalRequiresAccepted ==
  \A g \in Gen : (terminalWinner[g] = 1) => committed[g]

\* Publication follows a terminal win.
InvPublishAfterTerminal ==
  \A g \in Gen : (published[g] >= 1) => (terminalWinner[g] >= 1)

\* Reap's publication handshake is indivisible: a published occupant is
\* completion_ready with closed registration, ended borrow, off-ring, and
\* decremented accounting (direct-publish witness).
InvPublishedCompleteness ==
  (published[gen] >= 1) =>
    /\ state = "completion_ready"
    /\ regState = "closed"
    /\ ~borrowActive
    /\ ~onRing
    /\ acceptedOutstanding = 0

\* Nothing is ever published while the enqueue pin is live (I19).
InvNoPinnedPublication == (published[gen] >= 1) => ~pin

\* A pinned backend_ready head is reap-ineligible AND unpublished.
InvPinReapEligibility ==
  (onRing /\ pin) => (state = "backend_ready" /\ published[gen] = 0)

\* Generation strictly advances past every freed occupant before reuse (I6).
InvGenAdvanceOnFree == \A g \in Gen : freed[g] => (g < gen)

\* A slot leaves occupancy only via pre-commit rollback or post-reap release.
InvReleasePath ==
  \A g \in Gen : freed[g] => ((published[g] >= 1) \/ ~committed[g])

\* Reap validated the publication binding before publishing (review C2).
InvReapRequiresBinding ==
  (published[gen] >= 1) => bindingInstalled

\* ENVIRONMENT-CONTRACT invariant (PR #125 review P1-2): a canceled terminal
\* was written only by Scheme-B pending/enqueued cancel (leaf-enforced via
\* the arena's own cancel() entry) or a CONFIRMED running interruption
\* (caller discipline — see RecordCanceledConfirmed). The leaf API does not
\* enforce the confirmed-interruption provenance; this law holds of the
\* modeled environment in which callers honor the Decision-11 obligation,
\* and NEG-RA-6 shows the violation an ill-behaved caller produces.
\* Decision 11 verbatim law: a running cancel that merely recorded intent
\* never yields "cancel_running".
InvCanceledTerminalSource ==
  /\ \A g \in Gen : cancelSource[g] # "cancel_running"
  /\ terminalCanceled => cancelSource[gen] \in LegitimateCancelSources

\* A freed slot carries no live per-occupant protocol state.
InvFreeClean ==
  state = "free" =>
    /\ ~pin
    /\ ~terminalStored
    /\ ~onRing
    /\ ~borrowActive
    /\ slotInUse = 0
    /\ acceptedOutstanding = 0

\* Destruction quiescence (AC-13), CHECKED independently of the Destroy
\* action guards (review nitpick): a destroyed arena is idle — free slot,
\* no slot in use, no accepted outstanding. The Destroy guards enforce this
\* on the transition; this invariant detects any future action that could
\* corrupt quiescence after destruction without relying on those guards.
InvDestroyQuiescent ==
  destroyed => ( /\ state = "free"
                 /\ slotInUse = 0
                 /\ acceptedOutstanding = 0 )

Inv ==
  /\ TypeOK
  /\ InvAccounting
  /\ InvBorrowWindow
  /\ InvTerminalStates
  /\ InvOnRingMatches
  /\ InvPinPhase
  /\ InvNoDoubleTerminal
  /\ InvNoDoublePublication
  /\ InvTerminalRequiresAccepted
  /\ InvPublishAfterTerminal
  /\ InvPublishedCompleteness
  /\ InvNoPinnedPublication
  /\ InvPinReapEligibility
  /\ InvGenAdvanceOnFree
  /\ InvReleasePath
  /\ InvReapRequiresBinding
  /\ InvCanceledTerminalSource
  /\ InvFreeClean
  /\ InvDestroyQuiescent

(***************************************************************************)
(* Liveness — CONDITIONAL on Layer-B external progress obligations          *)
(* (PR #125 review P1-1). What is proven here is:                           *)
(*                                                                          *)
(*   IF the backend submit path fairly executes the post-commit enqueue     *)
(*      (WF(Enqueue): mandatory, allocation-free, noexcept — AGENTS 10.2;   *)
(*      bound C++ site: ThreadPoolBackend::enqueue_after_commit)           *)
(*   AND the backend/runtime progress loop fairly executes reap            *)
(*      (WF(Reap): level-triggered from poll/wait_one/reaper paths,        *)
(*      AGENTS 13.2; bound C++ sites: ThreadPoolBackend::poll/wait_one,    *)
(*      UringAsyncBackend reaper)                                          *)
(*   THEN the leaf protocol converges (no lost reap readiness, every       *)
(*      pin acknowledged).                                                 *)
(*                                                                          *)
(* The arena leaf itself guarantees NEITHER: nothing inside                *)
(* request_arena.hpp obliges anyone to call enqueue or reap. These are     *)
(* assumptions about OTHER code, which is why every cfg sets               *)
(* CHECK_DEADLOCK FALSE — terminal states (a destroyed arena, an idle free *)
(* slot with no caller) legitimately have no enabled action, and           *)
(* deadlock-freedom is NOT a claim of this leaf model. Also deliberately   *)
(* NOT assumed: backend/syscall termination (an accepted request reaching  *)
(* a terminal is an environment assumption, as in the blocking-io-pool     *)
(* suite) and scheduler worker fairness. A compositional                  *)
(* RequestArena + backend-progress refinement is recorded as debt in the   *)
(* coverage matrix rather than stuffed into this leaf.                     *)
(***************************************************************************)

LivenessSpec == Spec /\ WF_vars(Enqueue) /\ WF_vars(Reap)

\* An acked backend_ready ring entry is eventually published (no lost reap
\* readiness; the pinned-head re-arm is level-triggered) — CONDITIONAL on
\* the WF(Reap) Layer-B obligation above.
BackendReadyEventuallyPublished ==
  []( (state = "backend_ready" /\ ~pin /\ onRing) =>
        <>(state = "completion_ready") )

\* A Scheme-B cancel-won pinned slot eventually gets its pin acknowledged
\* (the submit path completes), unblocking reap — CONDITIONAL on the
\* WF(Enqueue) Layer-B obligation above.
PinEventuallyAcknowledged ==
  []( (pin /\ state = "backend_ready") => <>(~pin) )

LivenessProps == /\ BackendReadyEventuallyPublished
                 /\ PinEventuallyAcknowledged

(***************************************************************************)
(* Reachability / non-vacuity witnesses (scene cfgs assert the NotReach_*  *)
(* form and expect TLC to VIOLATE it — the scene is reachable).            *)
(***************************************************************************)

\* W1: Scheme-B cancel won while pending; the enqueue pin is still live.
NotReach_W1_SchemeBCancelWonPending ==
  ~( /\ state = "backend_ready"
     /\ pin
     /\ terminalCanceled
     /\ cancelSource[gen] = "cancel_pending" )

\* W2: the submit path's enqueue observed the canceled terminal and
\* acknowledged the pin (successful terminal_noop).
NotReach_W2_EnqueueNoopAfterCancel ==
  ~( /\ state = "backend_ready"
     /\ ~pin
     /\ onRing
     /\ terminalCanceled
     /\ cancelSource[gen] = "cancel_pending" )

\* W3: an ordinary result was recorded VERBATIM after a running cancel
\* intent (Decision 11: intent does not rewrite the result).
NotReach_W3_OrdinaryVerbatimAfterIntent ==
  ~( /\ terminalStored
     /\ ~terminalCanceled
     /\ intentSeen[gen]
     /\ terminalWinner[gen] = 1 )

\* W4: the slot was reused by a second committed occupant — stale keys of
\* the previous generation race against it.
NotReach_W4_ReusedSlotCommitted ==
  ~(gen >= 1 /\ committed[gen])

\* W5: a registered waiter was delivered exactly-once by reap (the
\* register-after-terminal-before-reap window).
NotReach_W5_WaiterDeliveredByReap == ~(waiterDelivered[gen] = TRUE)

=============================================================================
