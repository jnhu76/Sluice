------------------------------- MODULE F1WaitRecord -------------------------------

(* F1WaitRecord - Phase-F1 WaitRecord registry generation/lease/delivery model.

Focused safety model for MODEL-007(b) (audit #162, umbrella #171, child #174):
the identity-bearing Scheduler waiter-registry protocol behind
`suspend-on-Completion` wake routing.

Race B (cancel_waiter vs reap delivery) - who owns the routing lease, and
does the loser stay inert (exactly-once fiber publication)?
Race C (record generation reuse) - can a stale token/ReadyEvent mutate or
wake the NEW occupant of a recycled WaitRecord?

C++ fact source (all line refs at branch baseline c1e93f9):
  - include/sluice/async/scheduler.hpp:1164-1300  (WaitRecord, states, sink)
  - src/async/scheduler_park_wake.cpp:684-990     (acquire/retire, register,
                                                   cancel_waiter)
  - src/async/scheduler.cpp:1340-1500             (drain, ReadyRoutingSink)
  - include/sluice/async/detail/request_arena.hpp:629-690 (arena waiter
                                                   registration leaf)
  - include/sluice/async/detail/ready_sink.hpp    (WaiterToken/RoutingLease)

Exactly-once publication layers (see issue #174 Comment A):
  L1 arena leaf       cancel XOR reap-extract owns the waiter delivery
  L2 sink state check marks Delivered only from state Registered
  L3 cancel check     retires only when generation matches AND state Registered
  L4 E7-T2 CAS        make_runnable publishes at most once (modeled as the
                      pub[w] = 0 guard; its removal is e7_publication's
                      modeled defect, not duplicated here)

Boundary: 1 record, 2 registration epochs, waiters {W0, W1}, one
delivery actor, one cancel actor. Generation values mirror the real C++
pool discipline (README "Generation mapping"): 0 = pool-construction
value, NEVER issued to any occupant (it is exactly the value a forged
stale token carries); the first acquire bumps 0->1 (W0's token); reuse
bumps 1->2 (W1's token). Safety only. Race A (pre-registration reap
window) is closed at the arena leaf and out of scope; the non-arena legacy
Completion*-map fallback is out of scope. Memory-model boundary: every
modeled transition happens under wait_registry_mtx_ or the arena leaf mutex
(both SC domains); make_runnable is a single CAS unit. TLC proves the SC
protocol abstraction only - no C++ weak-memory claim.

Fused vs split (lock scoping is the authority):
  FUSED  registration (record acquire + arena register_waiter: the window
         between them can only race a no-waiter reap - Race A, out of
         scope); cancel retire+publish and drain pop+retire+publish each run
         inside one global_mtx_ scope in the C++; new-slot rearm into
         epoch-1 acquire (W1's slot is a fresh slot object in the C++).
  SPLIT  cancel arena-CS vs reap extract (independent threads, arena leaf
         arbitrates); extract vs sink-mark (the sink runs on any poll
         thread with NO Scheduler lock - tests/scheduler_identity_wake_
         test.cpp T5 polls from a bare thread); sink-mark vs cancel-retire
         vs drain (registry leaf only).

Init uses equality form on every variable: TLC 2.19's initial-state
enumerator rejects negation-form constraints with a misleading
"identifier undefined" diagnostic (issue #172 lesson). *)

EXTENDS Naturals, FiniteSets

CONSTANTS
    Waiters,                  \* set of modeled waiters {W0, W1}
    W0, W1,                   \* model values (epoch-0 / epoch-1 occupants)
    CheckGeneration,          \* NEG-WR1: FALSE drops the sink's generation check
    CancelClearsDelivery,     \* NEG-WR3: FALSE = cancel keeps waiter_delivery_present_ set
    DeliveredPinBlocksReuse   \* NEG-WR2: FALSE = acquire may take a delivered-pinned record

VARIABLES
    generation,       \* WaitRecord generation; bumped inside the acquire
                      \* critical section before the new occupant is visible
    recordState,      \* Free / Registered / Delivered (cancel writes
                      \* cancelled-then-free inside ONE registry CS in the
                      \* C++ - externally unobservable, modeled Free)
    occupant,         \* None / W0 / W1 (the record's registered waiter)
    slotWaiter,       \* None / W0 / W1 (the ARENA SLOT's current waiter -
                      \* the waiter_token_ identity carried by the slot
                      \* registration, NOT the WaitRecord occupant: cancel/
                      \* extract act on the slot (request_arena.hpp
                      \* cancel_waiter/reap, keyed by SlotHandle), so a slot
                      \* reopened by cancel still names its waiter after the
                      \* record was retired. Decoupled so the sequential
                      \* double-grant schedule (cancel granted -> cancel
                      \* consumed -> late reap extract) stays expressible -
                      \* the historical-XOR evidence for InvSingleAuthority)
    slotReg,          \* arena-slot waiter registration for the current epoch:
                      \* Fresh (accepted, no waiter) / Registered (lease in
                      \* slot) / Open (cancel reopened) / Closed (reaped)
    deliveryPresent,  \* the arena slot's waiter_delivery_present_ flag
    authDeliv,        \* delivery authority (token+lease) in flight:
                      \* None / Held
    delivGen,         \* generation stamped on the delivery at extraction
    authCancel,       \* cancel authority (lease) in flight: None / Held
    cancelGen,        \* generation stamped on the cancel lease
    deliveredGen,     \* generation stamped at the sink's delivered-marking
                      \* (3 = NoDelivered sentinel; 0/1/2 are real
                      \* generation values, so the sentinel cannot collide)
    liveCount,        \* P1-2 wait_record_live_count_ (registry accounting)
    pub,              \* runnable publications per waiter (E7-T2 CAS gate)
    outcome,          \* P1-1 frozen outcome per waiter
    authorityGrants,  \* HISTORY (no action guard): per-waiter set of
                      \* authority kinds granted at the arena leaf in this
                      \* waiter's registration epoch, SUBSET {Cancel,
                      \* Delivery}. Waiter and epoch are 1:1 in this
                      \* boundary (each waiter registers at most once), so
                      \* per-waiter keying IS per-epoch keying here; no
                      \* re-arm is needed on reuse.
    sawCancelWon,     \* history witness: cancel won the arena race
    sawDeliveryWon,   \* history witness: reap-extract took the delivery
    sawStaleDropped,  \* history witness: a stale event was inertly dropped
    sawDeliveryAfterCancel
                      \* history witness: a delivery grant landed in an
                      \* epoch whose grant set already held Cancel (the
                      \* sequential double-grant shape)

vars ==
    <<generation, recordState, occupant, slotWaiter, slotReg, deliveryPresent,
      authDeliv, delivGen, authCancel, cancelGen, deliveredGen, liveCount,
      pub, outcome, authorityGrants, sawCancelWon, sawDeliveryWon,
      sawStaleDropped, sawDeliveryAfterCancel>>

\* Real C++ pool discipline: construction generation 0 is never issued;
\* the first acquire bumps 0->1, reuse bumps 1->2.
GenOf(w) == IF w = W0 THEN 1 ELSE 2

Init ==
    /\ generation = 0
    /\ recordState = "Free"
    /\ occupant = "None"
    /\ slotWaiter = "None"
    /\ slotReg = "Fresh"
    /\ deliveryPresent = FALSE
    /\ authDeliv = "None"
    /\ delivGen = 0
    /\ authCancel = "None"
    /\ cancelGen = 0
    /\ deliveredGen = 3
    /\ liveCount = 0
    /\ pub = [w \in Waiters |-> 0]
    /\ outcome = [w \in Waiters |-> "None"]
    /\ authorityGrants = [w \in Waiters |-> {}]
    /\ sawCancelWon = FALSE
    /\ sawDeliveryWon = FALSE
    /\ sawStaleDropped = FALSE
    /\ sawDeliveryAfterCancel = FALSE

(* A1 - registration, fused: record acquire (generation bumped INSIDE the
   critical section = bump-before-visibility) + arena register_waiter (slot
   Fresh->Registered, delivery present). W1 additionally requires the
   epoch-0 slot reaped (Closed) AND generation = 1 - the leftover from W0's
   acquire bump 0->1, i.e. W0's epoch settled; reuse then bumps 1->2:
   sequential-epoch boundary - a concurrent
   second slot adds no new registry-race shape (T6 reuses after A settles).
   NEG-WR2: the mutant may pop a record that is still delivered-pinned
   (recycle before the drain closed the lease). *)
AcquireRegister(w) ==
    /\ \/ recordState = "Free"
       \/ /\ recordState = "Delivered"
          /\ DeliveredPinBlocksReuse = FALSE
    /\ authDeliv = "None"
    /\ authCancel = "None"
    /\ IF w = W0
         THEN slotReg = "Fresh"
         ELSE /\ slotReg = "Closed"
              /\ generation = 1
    /\ generation' = GenOf(w)
    /\ recordState' = "Registered"
    /\ occupant' = w
    /\ slotWaiter' = w
    /\ slotReg' = "Registered"
    /\ deliveryPresent' = TRUE
    /\ liveCount' = liveCount + 1
    /\ UNCHANGED <<authDeliv, delivGen, authCancel, cancelGen, deliveredGen,
                   pub, outcome, authorityGrants, sawCancelWon,
                   sawDeliveryWon, sawStaleDropped, sawDeliveryAfterCancel>>

(* B1 - cancel wins the arena leaf race: the lease moves out to the caller
   and the slot registration reopens (the I/O itself is untouched - I5).
   As-built clears waiter_delivery_present_ here; NEG-WR3 keeps it set,
   which lets a later reap ALSO extract the delivery (broken L1
   arbitration). The grant is recorded on the per-epoch authority history
   (the historical-XOR evidence for InvSingleAuthority). *)
ArenaCancelWin(w) ==
    /\ slotReg = "Registered"
    /\ slotWaiter = w
    /\ slotReg' = "Open"
    /\ deliveryPresent' = IF CancelClearsDelivery THEN FALSE ELSE TRUE
    /\ authCancel' = "Held"
    /\ cancelGen' = generation
    /\ authorityGrants' =
         [authorityGrants EXCEPT ![w] = authorityGrants[w] \cup {"Cancel"}]
    /\ sawCancelWon' = TRUE
    /\ UNCHANGED <<generation, recordState, occupant, slotWaiter, authDeliv,
                   delivGen, deliveredGen, liveCount, pub, outcome,
                   sawDeliveryWon, sawStaleDropped, sawDeliveryAfterCancel>>

(* C1 - reap closes the slot registration and takes the waiter delivery
   exactly-once (iff still present). Keyed on the SLOT's waiter identity
   (slotWaiter), not the record occupant: the C++ extract acts on the slot
   (request_arena.hpp reap), so it stays enabled after the record was
   retired by cancel - the schedule that exposes the sequential
   double-grant. From Open (post-cancel) the as-built extraction carries
   has_waiter=false and the sink no-ops. A real grant is recorded on the
   per-epoch authority history; the sequential double-grant ghost fires
   only when this grant lands after a Cancel grant in the SAME epoch
   (witness for NoReachSequentialDoubleGrant - history only, never a
   guard). *)
ArenaReapExtract(w) ==
    /\ slotWaiter = w
    /\ slotReg \in {"Registered", "Open"}
    /\ slotReg' = "Closed"
    /\ slotWaiter' = "None"
    /\ IF deliveryPresent
         THEN /\ authDeliv' = "Held"
              /\ delivGen' = generation
              /\ deliveryPresent' = FALSE
              /\ sawDeliveryWon' = TRUE
              /\ authorityGrants' =
                   [authorityGrants EXCEPT ![w] = authorityGrants[w] \cup {"Delivery"}]
              /\ sawDeliveryAfterCancel' =
                   IF "Cancel" \in authorityGrants[w]
                     THEN TRUE
                     ELSE sawDeliveryAfterCancel
         ELSE /\ UNCHANGED <<authDeliv, delivGen, deliveryPresent,
                             sawDeliveryWon, authorityGrants,
                             sawDeliveryAfterCancel>>
    /\ UNCHANGED <<generation, recordState, occupant, authCancel, cancelGen,
                   deliveredGen, liveCount, pub, outcome, sawCancelWon,
                   sawStaleDropped>>

(* C2 - ReadyRoutingSink::on_ready consumes the delivery authority.
   Validation order mirrors the C++ (scheduler_park_wake/scheduler.cpp):
   the generation comparison (CheckGeneration; identity/slot-bound checks
   are parallel stale-drop branches, collapsed here to the generation
   check), then the L2 state check. Every path consumes the lease. *)
SinkMarkDelivered ==
    /\ authDeliv = "Held"
    /\ authDeliv' = "None"
    /\ IF (delivGen = generation) \/ (CheckGeneration = FALSE)
         THEN /\ recordState' =
                  IF recordState = "Registered" THEN "Delivered" ELSE recordState
              /\ deliveredGen' =
                  IF recordState = "Registered" THEN delivGen ELSE deliveredGen
              /\ UNCHANGED sawStaleDropped
         ELSE /\ sawStaleDropped' = TRUE
              /\ UNCHANGED <<recordState, deliveredGen>>
    /\ UNCHANGED <<generation, occupant, slotWaiter, slotReg, deliveryPresent,
                   authCancel, delivGen, cancelGen, liveCount, pub, outcome,
                   authorityGrants, sawCancelWon, sawDeliveryWon,
                   sawDeliveryAfterCancel>>

(* T6 - a forged/stale gen-0 event injected directly at the sink while the
   reused record holds epoch-1's occupant (tests/scheduler_identity_wake_
   test.cpp f1_stale_record_generation_no_wake does exactly this). The
   event carries no authority variable: the sink simply validates and
   drops. Generation 0 is the pool-construction value NEVER issued to an
   occupant (first acquire bumps 0->1, reuse 1->2), so the forged token is
   stale against EVERY real occupant; the current occupant W1 holds
   generation 2, and the comparison for THIS event is 0 = 2. *)
SinkStaleGen0 ==
    /\ generation = 2
    /\ occupant = W1
    /\ recordState = "Registered"
    /\ IF (0 = generation) \/ (CheckGeneration = FALSE)
         THEN /\ recordState' = "Delivered"
              /\ deliveredGen' = 0
              /\ UNCHANGED sawStaleDropped
         ELSE /\ sawStaleDropped' = TRUE
              /\ UNCHANGED <<recordState, deliveredGen>>
    /\ UNCHANGED <<generation, occupant, slotWaiter, slotReg, deliveryPresent,
                   authDeliv, delivGen, authCancel, cancelGen, liveCount, pub,
                   outcome, authorityGrants, sawCancelWon, sawDeliveryWon,
                   sawDeliveryAfterCancel>>

(* C3 - drain consume + publish, one global_mtx_ scope in the C++ (pop the
   delivered list, retire to free, freeze outcome=completed, make_runnable,
   route). The drain pops the RECORD (intrusive delivered link) - no token
   re-validation in the C++; the E7-T2 CAS (pub[w] = 0) is the publication
   gate. P1-1 as-built ORDER (scheduler.cpp:1385-1391): freeze the outcome
   BEFORE make_runnable, UNCONDITIONALLY - set_completion_wait_outcome is
   noexcept and has no failure branch; only the publication is CAS-gated
   (a concurrent already-runnable wake could lose the CAS AFTER the
   freeze). *)
DrainConsumePublish(w) ==
    /\ recordState = "Delivered"
    /\ occupant = w
    /\ recordState' = "Free"
    /\ occupant' = "None"
    /\ liveCount' = liveCount - 1
    /\ outcome' = [outcome EXCEPT ![w] = "Completed"]
    /\ pub' = IF pub[w] = 0
                THEN [pub EXCEPT ![w] = 1]
                ELSE pub
    /\ UNCHANGED <<generation, slotWaiter, slotReg, deliveryPresent, authDeliv,
                   delivGen, authCancel, cancelGen, deliveredGen,
                   authorityGrants, sawCancelWon, sawDeliveryWon,
                   sawStaleDropped, sawDeliveryAfterCancel>>

(* B2 - cancel retire + publish, one global_mtx_ scope in the C++. The L3
   check (generation matches AND state Registered) mirrors the production
   cancel body; on failure the lease is dropped with NO publication
   (defense-in-depth - unreachable while L1 holds). The published waiter is
   read from the record (occupant = w). P1-1 as-built ORDER
   (scheduler_park_wake.cpp:946-951): freeze canceled UNCONDITIONALLY
   before the make_runnable CAS; publication is CAS-gated. *)
CancelRetirePublish(w) ==
    /\ authCancel = "Held"
    /\ occupant = w
    /\ authCancel' = "None"
    /\ IF (cancelGen = generation) /\ (recordState = "Registered")
         THEN /\ recordState' = "Free"
              /\ occupant' = "None"
              /\ liveCount' = liveCount - 1
              /\ outcome' = [outcome EXCEPT ![w] = "Canceled"]
              /\ pub' = IF pub[w] = 0
                          THEN [pub EXCEPT ![w] = 1]
                          ELSE pub
         ELSE /\ UNCHANGED <<recordState, occupant, liveCount, pub, outcome>>
    /\ UNCHANGED <<generation, slotWaiter, slotReg, deliveryPresent, authDeliv,
                   delivGen, cancelGen, deliveredGen, authorityGrants,
                   sawCancelWon, sawDeliveryWon, sawStaleDropped,
                   sawDeliveryAfterCancel>>

Stutter == UNCHANGED vars

Next ==
    \/ \E w \in Waiters :
         \/ AcquireRegister(w)
         \/ ArenaCancelWin(w)
         \/ ArenaReapExtract(w)
         \/ DrainConsumePublish(w)
         \/ CancelRetirePublish(w)
    \/ SinkMarkDelivered
    \/ SinkStaleGen0
    \/ Stutter

Spec == Init /\ [][Next]_vars

(* ---- As-built safety laws ---- *)

(* Race C: a delivered-marking belongs to the CURRENT occupant's generation.
   Violated by NEG-WR1 (stale event marks the reused record). *)
InvGenerationIsolation ==
    (recordState = "Delivered") => (deliveredGen = generation)

(* L1 / C2c: the arena leaf grants the waiter delivery to exactly one of
   cancel / reap-extract — a HISTORICAL XOR per registration epoch: even
   after the first grant is CONSUMED (lease retired, authority back to
   None), a second grant of the other kind in the same epoch is an
   arbitration break. The earlier simultaneous-only form
   (~(authDeliv=Held /\ authCancel=Held)) let the sequential schedule
   (cancel wins -> cancel consumed -> later reap extracts the kept
   delivery) pass undetected - never simultaneously held, yet historical
   exclusivity broken. Violated by NEG-WR3 in BOTH shapes. *)
InvSingleAuthority ==
    \A w \in Waiters : Cardinality(authorityGrants[w]) <= 1

(* E7-T2: at most one runnable publication per waiter. *)
InvSingleDelivery ==
    \A w \in Waiters : pub[w] \leq 1

(* The in-slot lease pins the record: while the slot holds the waiter
   registration, the record is live (never retired/reused). *)
InvSlotLeasePinsRecord ==
    (slotReg = "Registered") => (recordState = "Registered")

(* An extracted-but-unconsumed authority (delivery in flight to the sink,
   or a won cancel lease in flight to the retire) pins the record
   Registered. *)
InvAuthorityPinsRecord ==
    ((authDeliv = "Held") \/ (authCancel = "Held")) =>
        (recordState = "Registered")

(* P1-2 pool accounting: exactly one live record iff it is not Free.
   wait_record_live_count_ is a production field checked at destruction. *)
InvLiveRecordAccounting ==
    liveCount = (IF recordState = "Free" THEN 0 ELSE 1)

(* P1-1 outcome freeze - the AS-BUILT contract is one-directional: a
   PUBLISHED waiter necessarily has a frozen outcome (the fiber reads the
   outcome after resume; the freeze precedes make_runnable in both
   terminal paths - scheduler.cpp:1385-1391, scheduler_park_wake.cpp:
   946-951). The converse (frozen outcome must publish) is NOT a C++
   guarantee: the freeze is unconditional (noexcept) while publication is
   E7-T2 CAS-gated, and a concurrent already-runnable wake could consume
   the CAS after the freeze. The earlier iff-form was therefore
   over-strong; the actions now model the exact as-built order (freeze,
   then CAS-gated publish). *)
InvOutcomeFrozenOnPublication ==
    \A w \in Waiters : (pub[w] = 1) => (outcome[w] # "None")

(* ---- Reachability witnesses (NoReach* invariants are deliberately false
   at the target states; TLC's CEX is the witness. Ghost variables are
   append-only history - never action guards. ---- *)

(* BR1: the arena-decided-but-unconsumed window exists. *)
NoReachAuthorityWindow ==
    ~ ((authDeliv = "Held") \/ (authCancel = "Held"))

(* BR1 both-ways coverage. *)
NoReachCancelWon == ~ sawCancelWon
NoReachDeliveryWon == ~ sawDeliveryWon

(* BR2: the record was recycled and re-registered at generation 2 (the
   reuse bump 1->2; W1 is the epoch-1 occupant). *)
NoReachReuse ==
    ~ ((generation = 2) /\ (occupant = W1) /\ (recordState = "Registered"))

(* BR3: a stale gen-0 event was processed during epoch 1 and inertly
   dropped by the generation check. *)
NoReachStaleDropped == ~ sawStaleDropped

(* BR4: the sequential double-grant shape - a delivery grant landed in an
   epoch whose grant set already held Cancel (the cancel authority may
   already be consumed). The critical historical-XOR counterexample shape
   that the old simultaneous-only property could not see; witnessed under
   the NEG-WR3 mutant so the strengthened law is proven non-vacuous
   against the exact sequential schedule, not only the overlap prefix. *)
NoReachSequentialDoubleGrant == ~ sawDeliveryAfterCancel
====
