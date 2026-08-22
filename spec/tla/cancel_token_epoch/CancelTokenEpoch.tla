------------------------------- MODULE CancelTokenEpoch -------------------------------

(* CancelTokenEpoch - the request-epoch protocol behind sluice::async
   cooperative task cancellation (MODEL-007c, audit #162 / umbrella #171 /
   child #180).

Focused safety model of the AS-BUILT CancelToken / CancelState / check_cancel
protocol (ADR-cancel-request-epoch, Accepted 2026-08-13, baseline 5e5ec36):

  CancelToken  : one atomic uint64 -- bit 0 = request-pending, bits 1..63 =
                 the request EPOCH (the identity of the current request).
  CancelState  : per-consumer protection bit + last-acknowledged epoch.

  request() : idle -> pending=1, epoch+=1   (idempotent no-op while pending)
  rearm()   : pending -> epoch+=1 (pending stays set; re-arms delivery)
              idle -> no-op
  clear()   : pending=0 (epoch unchanged; a later request() is a NEW request)
  check_cancel(token, state) : ONE atomic snapshot; delivers IoError::canceled
              iff unblocked AND pending AND state.acked_epoch != token.epoch;
              on delivery records state.acked_epoch = token.epoch (single-shot
              per request).

C++ fact source (branch baseline e035ff5):
  - include/sluice/async/cancel.hpp   (CancelToken/CancelState/CancelGuard/check_cancel)
  - src/async/cancel.cpp              (request/rearm/clear/acknowledged/check_cancel)
  - include/sluice/async/future.hpp   (Future::cancel = token_.request(); producer CancelState)
  - include/sluice/async/group.hpp    (Group::cancel = token_.request(); one shared token)

The C++-first recovery (issue #180 Comment A) established that this is a
REQUEST-epoch protocol, NOT a task-incarnation epoch protocol: the token is a
long-lived shareable object (a Group shares ONE token across its tasks) and
"reuse" is token reuse (clear() + a new request()) plus per-consumer
acknowledgement across request generations. The model boundary therefore uses
1 token, 2 consumers {A, B} (the shared-token per-consumer delivery), and
request generations 0..MaxEpoch - not the hypothetical per-incarnation tokens
of the original MODEL-007(c) brief.

Boundary: 1 token, 2 consumers, 1 canceller, request generations 0..3, safety
only. reset_acknowledgement() (the per-consumer re-arm) is OUT of scope: it is
the per-consumer variant of token-side rearm() and would deliberately re-open
the same epoch to a second delivery, obscuring the single-shot law; token-side
rearm() covers the re-arm mechanism. The Future/Group task machinery, the
Scheduler, wait registration, and backend op cancel (ADR X2/X3) are out of
scope (e16 models the runtime; the token is the T1 authority).

Memory-model boundary: token.state_ is ONE atomic uint64; request/rearm use a
release CAS, clear a release fetch_and, is_requested/epoch/check_cancel an
acquire load. check_cancel reads ONE snapshot, so a delivery linearizes at a
single moment (a concurrent clear() only affects later checks). The
per-consumer CancelState fields are plain, consumer-owned (single-threaded in
production). TLC proves the SC protocol abstraction only - no C++ weak-memory
claim.

Init uses equality form on every variable (TLC 2.19's initial-state enumerator
rejects negation-form constraints - issue #172 lesson). *)

EXTENDS Naturals, FiniteSets

CONSTANTS
    Consumers,          \* modeled consumers {A, B} sharing the token
    A, B,               \* model values
    AckIsSticky,        \* NEG-CT1: FALSE = acknowledgement is a sticky bool
                        \*   (the pre-fix representation; ack never becomes
                        \*   request-relative)
    ClearClearsPending, \* NEG-CT2: FALSE = clear() keeps the pending bit
    SingleShot,         \* NEG-CT3: FALSE = every unblocked pending check
                        \*   delivers (single-shot ack gate dropped)
    CheckPending,       \* NEG-CT4: FALSE = delivery ignores the pending bit
    ProtectionBlocks    \* NEG-CT5: FALSE = a blocked consumer still delivers

\* Focused request-generation bound (traces need up to epoch 3: request,
\* rearm, rearm before the first cancel point - the NEG-CT1 CEX) and clear
\* bound (one clear suffices for every trace; a second clear is permitted for
\* the double-clear idempotence shape). Definitions, not constants, so the
\* per-gate cfgs stay minimal.
MaxEpoch == 3
MaxClear == 2

VARIABLES
    pending,              \* token pending bit (state_ bit 0)
    epoch,                \* token request epoch (state_ bits 1..63)
    postClear,            \* a clear() happened and no request() since (the
                          \*   "reuse phase" marker: a cleared token is idle)
    requestedEpochs,      \* HISTORY: epochs created as pending requests by
                          \*   request()/rearm() (guard-free provenance)
    acked,                \* per-consumer last-acknowledged epoch
                          \*   (0 = none; NEG-CT1: 0/1 sticky flag)
    blocked,              \* per-consumer CancelProtection (FALSE = unblocked)
    lastCheckEpoch,       \* HISTORY: the token epoch at the consumer's most
                          \*   recent UNBLOCKED cancel point (observation
                          \*   record; a protected check observes nothing)
    deliveredEpochs,      \* HISTORY: epochs the consumer has actually
                          \*   delivered (independent provenance)
    dupDelivered,         \* HISTORY: a consumer delivered the same epoch
                          \*   twice (single-shot witness)
    sawDeliveryWhileIdle, \* HISTORY: a delivery happened with pending=0
                          \*   (provenance witness - no ghost delivery)
    sawBlockedDelivery,   \* HISTORY: a delivery happened to a blocked
                          \*   consumer (protection witness)
    sawProtectedRequestDelivered,
                          \* HISTORY: a request that was once protected
                          \*   (blocked check while pending) later delivered
                          \*   (protection-blocks-delivery-not-request)
    blockedCheckedEpochs, \* HISTORY: epochs whose pending request was
                          \*   observed by a blocked cancel point
    sawDeliverAfterRearm, \* HISTORY: a delivery at or past a rearm with no
                          \*   intervening clear (rearm re-delivery witness)
    rearmEpoch,           \* HISTORY: epoch at the most recent rearm (0=none)
    clearSinceRearm,      \* HISTORY: a clear happened after the last rearm
    clearCount            \* HISTORY: number of clear() calls

vars ==
    <<pending, epoch, postClear, requestedEpochs, acked, blocked,
      lastCheckEpoch, deliveredEpochs, dupDelivered, sawDeliveryWhileIdle,
      sawBlockedDelivery, sawProtectedRequestDelivered, blockedCheckedEpochs,
      sawDeliverAfterRearm, rearmEpoch, clearSinceRearm, clearCount>>

Init ==
    /\ pending = FALSE
    /\ epoch = 0
    /\ postClear = FALSE
    /\ requestedEpochs = {}
    /\ acked = [c \in Consumers |-> 0]
    /\ blocked = [c \in Consumers |-> FALSE]
    /\ lastCheckEpoch = [c \in Consumers |-> 0]
    /\ deliveredEpochs = [c \in Consumers |-> {}]
    /\ dupDelivered = FALSE
    /\ sawDeliveryWhileIdle = FALSE
    /\ sawBlockedDelivery = FALSE
    /\ sawProtectedRequestDelivered = FALSE
    /\ blockedCheckedEpochs = {}
    /\ sawDeliverAfterRearm = FALSE
    /\ rearmEpoch = 0
    /\ clearSinceRearm = FALSE
    /\ clearCount = 0

(* request() from idle: pending=1 and epoch+=1 in ONE CAS (cancel.cpp:28) -
   the epoch advance and the pending bit are atomically co-visible, which is
   why the "epoch advanced too late" hypothesis (HC2) has no observable
   window in the as-built protocol. Idempotent: while pending the action is
   not enabled (a no-op request() writes nothing in the C++). *)
Request ==
    /\ ~pending
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ pending' = TRUE
    /\ postClear' = FALSE
    /\ requestedEpochs' = requestedEpochs \cup {epoch + 1}
    /\ UNCHANGED <<acked, blocked, lastCheckEpoch, deliveredEpochs,
                   dupDelivered, sawDeliveryWhileIdle, sawBlockedDelivery,
                   sawProtectedRequestDelivered, blockedCheckedEpochs,
                   sawDeliverAfterRearm, rearmEpoch, clearSinceRearm, clearCount>>

(* rearm() while pending (cancel.cpp:47): the SAME request stays pending and
   the epoch advances (Zig Io.recancel), so every consumer whose last delivery
   predates the new epoch delivers once more. Idle rearm is a no-op (not
   enabled). *)
Rearm ==
    /\ pending
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ requestedEpochs' = requestedEpochs \cup {epoch + 1}
    /\ rearmEpoch' = epoch + 1
    /\ clearSinceRearm' = FALSE
    /\ UNCHANGED <<pending, postClear, acked, blocked, lastCheckEpoch,
                   deliveredEpochs, dupDelivered, sawDeliveryWhileIdle,
                   sawBlockedDelivery, sawProtectedRequestDelivered,
                   blockedCheckedEpochs, sawDeliverAfterRearm, clearCount>>

(* clear() (cancel.cpp:64): pending=0, epoch unchanged; a later request() is
   a NEW request. NEG-CT2 flips ClearClearsPending so the pending bit
   survives - the "cancel intent survives reuse" defect (HC3): after a clear
   the old request still delivers to the next cancel point. *)
Clear ==
    /\ clearCount < MaxClear
    /\ pending' = IF ClearClearsPending THEN FALSE ELSE pending
    /\ postClear' = TRUE
    /\ clearCount' = clearCount + 1
    /\ clearSinceRearm' = TRUE
    /\ UNCHANGED <<epoch, requestedEpochs, acked, blocked, lastCheckEpoch,
                   deliveredEpochs, dupDelivered, sawDeliveryWhileIdle,
                   sawBlockedDelivery, sawProtectedRequestDelivered,
                   blockedCheckedEpochs, sawDeliverAfterRearm, rearmEpoch>>

(* CancelProtection::swap_protection - the consumer drives its own bit. *)
Protect(c) ==
    /\ blocked' = [blocked EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<pending, epoch, postClear, requestedEpochs, acked,
                   lastCheckEpoch, deliveredEpochs, dupDelivered,
                   sawDeliveryWhileIdle, sawBlockedDelivery,
                   sawProtectedRequestDelivered, blockedCheckedEpochs,
                   sawDeliverAfterRearm, rearmEpoch, clearSinceRearm, clearCount>>

Unprotect(c) ==
    /\ blocked' = [blocked EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<pending, epoch, postClear, requestedEpochs, acked,
                   lastCheckEpoch, deliveredEpochs, dupDelivered,
                   sawDeliveryWhileIdle, sawBlockedDelivery,
                   sawProtectedRequestDelivered, blockedCheckedEpochs,
                   sawDeliverAfterRearm, rearmEpoch, clearSinceRearm, clearCount>>

(* The as-built delivery decision, as one operator: deliver iff unblocked
   (and protection is enforced), the single-shot ack gate passes, and the
   pending gate passes. NEG-CT1..5 flip the corresponding switches. *)
DeliverNow(c) ==
    ~(blocked[c] /\ ProtectionBlocks)
      /\ (IF SingleShot
            THEN IF AckIsSticky THEN acked[c] # epoch ELSE acked[c] = 0
            ELSE TRUE)
      /\ (IF CheckPending THEN pending ELSE TRUE)

(* check_cancel(token, state) - ONE action, mirroring the single atomic
   snapshot (cancel.cpp:102): the delivery decision linearizes at a single
   moment. As-built (all switches TRUE) delivers iff:
     unblocked AND pending AND acked[c] # epoch
   and on delivery records acked[c] = epoch (single-shot per request). The
   ghosts record independent facts (token epoch, whether a request was
   pending, whether a delivery happened, blocked observations) - never
   self-labels. Every primed variable is assigned at the action's top-level
   conjunction (TLC requires assignments at that level, not inside a
   LET-IN expression). *)

CancelPoint(c) ==
    \* lastCheckEpoch records the epoch at the consumer's most recent
    \* UNBLOCKED cancel point: a protected region deliberately observes no
    \* cancel points (CancelProtection), so a blocked check must not advance
    \* the observation record.
    /\ lastCheckEpoch' =
         IF blocked[c] /\ ProtectionBlocks
           THEN lastCheckEpoch
           ELSE [lastCheckEpoch EXCEPT ![c] = epoch]
    /\ blockedCheckedEpochs' =
         IF blocked[c] /\ ProtectionBlocks /\ pending
           THEN blockedCheckedEpochs \cup {epoch}
           ELSE blockedCheckedEpochs
    /\ acked' = [acked EXCEPT ![c] =
                    IF DeliverNow(c)
                      THEN IF AckIsSticky THEN epoch ELSE 1
                      ELSE acked[c]]
    /\ deliveredEpochs' = [deliveredEpochs EXCEPT ![c] =
                               IF DeliverNow(c)
                                 THEN deliveredEpochs[c] \cup {epoch}
                                 ELSE deliveredEpochs[c]]
    \* All history ghosts use IF-form assignments (a top-level disjunction
    \* RHS - x' = a \/ b - defeats TLC 2.19's assignment detection).
    /\ dupDelivered' =
         IF DeliverNow(c) /\ epoch \in deliveredEpochs[c]
           THEN TRUE
           ELSE dupDelivered
    /\ sawDeliveryWhileIdle' =
         \* Fires iff a delivery happened while no request was pending.
         \* With the pending gate intact (CheckPending=TRUE) DeliverNow
         \* requires pending, so this never fires; the NEG-CT4 mutant
         \* (CheckPending=FALSE) makes an idle delivery reachable.
         IF DeliverNow(c) /\ ~pending
           THEN TRUE
           ELSE sawDeliveryWhileIdle
    /\ sawBlockedDelivery' =
         IF DeliverNow(c) /\ blocked[c]
           THEN TRUE
           ELSE sawBlockedDelivery
    /\ sawProtectedRequestDelivered' =
         IF DeliverNow(c) /\ epoch \in blockedCheckedEpochs
           THEN TRUE
           ELSE sawProtectedRequestDelivered
    /\ sawDeliverAfterRearm' =
         IF DeliverNow(c) /\ rearmEpoch >= 1 /\ ~clearSinceRearm
              /\ epoch >= rearmEpoch
           THEN TRUE
           ELSE sawDeliverAfterRearm
    /\ UNCHANGED <<pending, epoch, postClear, requestedEpochs, blocked,
                   rearmEpoch, clearSinceRearm, clearCount>>

Stutter == UNCHANGED vars

Next ==
    \/ Request
    \/ Rearm
    \/ Clear
    \/ \E c \in Consumers :
         \/ Protect(c)
         \/ Unprotect(c)
         \/ CancelPoint(c)
    \/ Stutter

Spec == Init /\ [][Next]_vars

(* ---- As-built safety laws (positive cfg, all PASS) ---- *)

(* Zig Io.zig:1186 single-shot per request: each consumer delivers a given
   request epoch at most once. dupDelivered is the history witness. *)
InvSingleShotPerEpoch == ~dupDelivered

(* Provenance: a delivery happens only while a request is actually pending
   (the anti-ghost law - no delivery without a real request to cause it).
   Violated by NEG-CT4. *)
InvNoDeliveryWhenIdle == ~sawDeliveryWhileIdle

(* CancelProtection blocks DELIVERY, never the request (Zig Io.zig:1322,
   ADR §2.2): a blocked cancel point observes nothing. Violated by NEG-CT5. *)
InvProtectionBlocksDelivery == ~sawBlockedDelivery

(* The "cancel intent does not survive reuse" law (HC3): after clear(), the
   token is idle until a new request() - a cleared token must not keep the
   old request pending for the next cancel point. Violated by NEG-CT2. *)
InvClearRemovesIntent == postClear => ~pending

(* The ADR's core semantic consequence: a consumer whose most recent
   UNBLOCKED cancel point already ran under the CURRENT pending request is
   unblocked now and has delivered it (a protected region observes no cancel
   points, so it never advances this record). This is the observable shape of
   the request-relative ack - a stale ack must not starve delivery of a newer
   request. Violated by NEG-CT1. *)
InvNoStaleAckStarvesDelivery ==
    \A c \in Consumers :
        (lastCheckEpoch[c] = epoch /\ pending /\ ~blocked[c])
          => (epoch \in deliveredEpochs[c])

(* Historical provenance (issue #175 lesson, history not just current state):
   every delivered epoch was a real created request. *)
InvDeliveredWasRequested ==
    \A c \in Consumers : deliveredEpochs[c] \subseteq requestedEpochs

(* The ADR's representation law: the acknowledgement is request-relative -
   it always equals a request epoch the consumer actually delivered (or 0).
   The pre-fix sticky-bool ack violates it (a first delivery at epoch # 1
   leaves acked=1 pointing at no delivered request). Violated by NEG-CT1. *)
InvAckIsRealEpoch ==
    \A c \in Consumers : acked[c] \in ({0} \cup deliveredEpochs[c])

(* ---- Reachability witnesses (NoReach* invariants are deliberately false at
   the target states; TLC's CEX is the witness. Ghost variables are
   append-only history - never action guards. ---- *)

(* CR1: a request was created (pending with epoch >= 1). *)
NoReachRequestCreated == ~(pending \/ epoch >= 1)

(* CR2: a consumer actually delivered a request. *)
NoReachDelivered == ~(\E c \in Consumers : deliveredEpochs[c] # {})

(* CR3: token reuse - after a consumer acked a request, clear()+request()
   opened a NEW request generation (epoch >= 2) while pending. *)
NoReachReuse ==
    ~((epoch >= 2) /\ pending /\ (\E c \in Consumers : acked[c] >= 1))

(* CR5: the ADR fix is non-vacuous - a consumer that PREVIOUSLY delivered an
   older request (Cardinality(deliveredEpochs[c]) >= 2, i.e. the current
   request is not the consumer's first) has delivered the NEW (>= 2) request
   after clear()+request(). The two-element history is what pins "token reuse
   for real": a fresh consumer that only ever delivered the current epoch
   (e.g. a rearm before its first check) cannot impersonate the reused path. *)
NoReachNewRequestDelivered ==
    ~(\E c \in Consumers : acked[c] = epoch /\ epoch >= 2 /\ pending
        /\ Cardinality(deliveredEpochs[c]) >= 2)

(* Shared-token semantics (Group): one request delivers to BOTH consumers -
   per-consumer acknowledgement is real, not a single global bit. *)
NoReachSharedDelivered ==
    ~(\E c1 \in Consumers : \E c2 \in Consumers :
         c1 # c2 /\ pending /\ epoch \in deliveredEpochs[c1]
         /\ epoch \in deliveredEpochs[c2])

(* Protection-blocks-delivery-not-request: a request observed by a blocked
   cancel point later delivers after unblocking (the request survived). *)
NoReachProtectedRequestDelivered == ~sawProtectedRequestDelivered

(* rearm() re-delivery (Zig Io.recancel): a consumer that already delivered a
   request delivers once more after rearm() - the request stays pending and
   its epoch advances. *)
NoReachRearmRedelivers == ~sawDeliverAfterRearm

(* CR6: clean reuse start - after clear(), the token is genuinely idle
   (pending=0) before the next request. *)
NoReachClearedIdle == ~(pending = FALSE /\ clearCount >= 1 /\ epoch >= 1)
====
