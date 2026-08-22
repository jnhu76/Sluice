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
  reset_acknowledgement() : per-consumer re-arm -- acked[c] := 0 (the next
              cancel point delivers the CURRENT request again even though
              this consumer already delivered it)
  check_cancel(token, state) : ONE atomic snapshot; delivers IoError::canceled
              iff unblocked AND pending AND state.acked_epoch != token.epoch;
              on delivery records state.acked_epoch = token.epoch (single-shot
              per request between explicit re-arm authorities).

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
only. reset_acknowledgement() (the per-consumer re-arm) IS modeled (review
fix, PR #181 round 2): the as-built CancelState carries an explicit
per-consumer re-arm authority (cancel.hpp:151, ADR-cancel-request-epoch
semantics table) that deliberately re-opens the SAME request epoch to a
second delivery; the single-shot law is therefore stated as "no duplicate
delivery WITHOUT an explicit re-arm authority" - token-side rearm() (a new
epoch) or per-consumer reset_acknowledgement() (acked := 0) - not as an
absolute per-epoch single-shot. The Future/Group task machinery, the
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
    AckIsEpochRelative, \* NEG-CT1: FALSE = acknowledgement is a sticky bool
                        \*   (the pre-fix representation; ack never becomes
                        \*   request-relative). TRUE = as-built epoch-relative.
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
                          \*   twice WITHOUT an explicit re-arm authority
                          \*   (single-shot witness)
    sawDeliveryWhileIdle, \* HISTORY: a delivery happened with pending=0
                          \*   (provenance witness - no ghost delivery)
    sawBlockedDelivery,   \* HISTORY: a delivery happened to a blocked
                          \*   consumer (protection witness)
    sawProtectedRequestDelivered,
                          \* HISTORY: the SAME consumer delivered an epoch it
                          \*   had previously observed from a blocked cancel
                          \*   point (protection-blocks-delivery-not-request)
    blockedCheckedEpochs, \* HISTORY (per consumer): the epoch whose pending
                          \*   request was observed by THAT consumer's most
                          \*   recent blocked cancel point (0 = none)
    sawDeliverAfterRearm, \* HISTORY: the SAME consumer that delivered the
                          \*   re-armed epoch delivers again after rearm()
                          \*   with no intervening clear (rearm witness)
    rearmEpoch,           \* HISTORY: epoch at the most recent rearm (0=none)
    clearSinceRearm,      \* HISTORY: a clear happened after the last rearm
    lastClearedEpoch,     \* HISTORY: the epoch effectively cleared by the
                          \*   most recent clear() that dropped a pending
                          \*   request (0 = none; reuse provenance for the
                          \*   clear+request witness)
    sawResetRedelivery,   \* HISTORY: a consumer re-delivered an epoch it had
                          \*   already delivered, after the explicit
                          \*   per-consumer ResetAcknowledgement re-arm
                          \*   (reset_acknowledgement witness)
    clearCount            \* HISTORY: number of clear() calls

vars ==
    <<pending, epoch, postClear, requestedEpochs, acked, blocked,
      lastCheckEpoch, deliveredEpochs, dupDelivered, sawDeliveryWhileIdle,
      sawBlockedDelivery, sawProtectedRequestDelivered, blockedCheckedEpochs,
      sawDeliverAfterRearm, rearmEpoch, clearSinceRearm, lastClearedEpoch,
      sawResetRedelivery, clearCount>>

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
    /\ blockedCheckedEpochs = [c \in Consumers |-> 0]
    /\ sawDeliverAfterRearm = FALSE
    /\ rearmEpoch = 0
    /\ clearSinceRearm = FALSE
    /\ lastClearedEpoch = 0
    /\ sawResetRedelivery = FALSE
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
                   sawDeliverAfterRearm, rearmEpoch,
                   clearSinceRearm, lastClearedEpoch, sawResetRedelivery,
                   clearCount>>

(* rearm() while pending (cancel.cpp:47): the SAME request stays pending and
   the epoch advances (Zig Io.recancel), so every consumer whose last delivery
   predates the new epoch delivers once more. Idle rearm is a no-op (not
   enabled). The epoch being re-armed (the pre-rearm epoch) is derived from
   rearmEpoch as RearmedFromEpoch, so the rearm witness can pin that the
   delivering consumer had actually delivered it. *)
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
                   blockedCheckedEpochs, sawDeliverAfterRearm, lastClearedEpoch,
                   sawResetRedelivery, clearCount>>

(* clear() (cancel.cpp:64): pending=0, epoch unchanged; a later request() is
   a NEW request. NEG-CT2 flips ClearClearsPending so the pending bit
   survives - the "cancel intent survives reuse" defect (HC3): after a clear
   the old request still delivers to the next cancel point. lastClearedEpoch
   records only an EFFECTIVE clear (one that drops a pending request): it is
   the reuse-history provenance the clear+request witness is pinned on. *)
Clear ==
    /\ clearCount < MaxClear
    /\ pending' = IF ClearClearsPending THEN FALSE ELSE pending
    /\ postClear' = TRUE
    /\ clearCount' = clearCount + 1
    /\ clearSinceRearm' = TRUE
    /\ lastClearedEpoch' =
         IF ClearClearsPending /\ pending
           THEN epoch
           ELSE lastClearedEpoch
    /\ UNCHANGED <<epoch, requestedEpochs, acked, blocked, lastCheckEpoch,
                   deliveredEpochs, dupDelivered, sawDeliveryWhileIdle,
                   sawBlockedDelivery, sawProtectedRequestDelivered,
                   blockedCheckedEpochs, sawDeliverAfterRearm, rearmEpoch,
                   sawResetRedelivery>>

(* CancelProtection::swap_protection - the consumer drives its own bit. *)
Protect(c) ==
    /\ blocked' = [blocked EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<pending, epoch, postClear, requestedEpochs, acked,
                   lastCheckEpoch, deliveredEpochs, dupDelivered,
                   sawDeliveryWhileIdle, sawBlockedDelivery,
                   sawProtectedRequestDelivered, blockedCheckedEpochs,
                   sawDeliverAfterRearm, rearmEpoch,
                   clearSinceRearm, lastClearedEpoch, sawResetRedelivery,
                   clearCount>>

Unprotect(c) ==
    /\ blocked' = [blocked EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<pending, epoch, postClear, requestedEpochs, acked,
                   lastCheckEpoch, deliveredEpochs, dupDelivered,
                   sawDeliveryWhileIdle, sawBlockedDelivery,
                   sawProtectedRequestDelivered, blockedCheckedEpochs,
                   sawDeliverAfterRearm, rearmEpoch,
                   clearSinceRearm, lastClearedEpoch, sawResetRedelivery,
                   clearCount>>

(* CancelState::reset_acknowledgement() (cancel.hpp:151) - the explicit
   per-consumer re-arm authority: the next cancel point delivers the CURRENT
   request again even though this consumer already delivered it (ADR
   semantics table; tests/cancel_token_test.cpp T-CANCEL-SHARED-4). It is
   consumer-owned, so it touches only acked[c]; it never changes the token. *)
ResetAcknowledgement(c) ==
    /\ acked' = [acked EXCEPT ![c] = 0]
    /\ UNCHANGED <<pending, epoch, postClear, requestedEpochs, blocked,
                   lastCheckEpoch, deliveredEpochs, dupDelivered,
                   sawDeliveryWhileIdle, sawBlockedDelivery,
                   sawProtectedRequestDelivered, blockedCheckedEpochs,
                   sawDeliverAfterRearm, rearmEpoch,
                   clearSinceRearm, lastClearedEpoch, sawResetRedelivery,
                   clearCount>>

(* The as-built delivery decision, as one operator: deliver iff unblocked
   (and protection is enforced), the single-shot ack gate passes, and the
   pending gate passes. NEG-CT1..5 flip the corresponding switches. *)
DeliverNow(c) ==
    ~(blocked[c] /\ ProtectionBlocks)
      /\ (IF SingleShot
            THEN IF AckIsEpochRelative THEN acked[c] # epoch ELSE acked[c] = 0
            ELSE TRUE)
      /\ (IF CheckPending THEN pending ELSE TRUE)

(* The epoch that the most recent rearm() re-armed (0 = none): rearmEpoch
   records the POST-rearm epoch (epoch+1), so the re-armed epoch is
   rearmEpoch - 1. Derived, not a state variable, to keep the state space
   small. *)
RearmedFromEpoch == IF rearmEpoch >= 1 THEN rearmEpoch - 1 ELSE 0

(* check_cancel(token, state) - ONE action, mirroring the single atomic
   snapshot (cancel.cpp:102): the delivery decision linearizes at a single
   moment. As-built (all switches TRUE) delivers iff:
     unblocked AND pending AND acked[c] # epoch
   and on delivery records acked[c] = epoch (single-shot per request between
   explicit re-arm authorities). The ghosts record independent facts (token
   epoch, whether a request was pending, whether a delivery happened, blocked
   observations) - never self-labels. Every primed variable is assigned at
   the action's top-level conjunction (TLC requires assignments at that
   level, not inside a LET-IN expression). *)

CancelPoint(c) ==
    \* lastCheckEpoch records the epoch at the consumer's most recent
    \* UNBLOCKED cancel point: a protected region deliberately observes no
    \* cancel points (CancelProtection), so a blocked check must not advance
    \* the observation record.
    /\ lastCheckEpoch' =
         IF blocked[c] /\ ProtectionBlocks
           THEN lastCheckEpoch
           ELSE [lastCheckEpoch EXCEPT ![c] = epoch]
    \* Per-consumer blocked-observation history: only consumer c's OWN
    \* blocked cancel point records the observed epoch into
    \* blockedCheckedEpochs[c] (scalar: the most recent blocked observation).
    /\ blockedCheckedEpochs' =
         [blockedCheckedEpochs EXCEPT ![c] =
             IF blocked[c] /\ ProtectionBlocks /\ pending
               THEN epoch
               ELSE blockedCheckedEpochs[c]]
    /\ acked' = [acked EXCEPT ![c] =
                    IF DeliverNow(c)
                      THEN IF AckIsEpochRelative THEN epoch ELSE 1
                      ELSE acked[c]]
    /\ deliveredEpochs' = [deliveredEpochs EXCEPT ![c] =
                               IF DeliverNow(c)
                                 THEN deliveredEpochs[c] \cup {epoch}
                                 ELSE deliveredEpochs[c]]
    \* dupDelivered: a delivery of the CURRENT epoch that was ALREADY
    \* delivered with no intervening explicit re-arm authority. Under the
    \* as-built ack (acked[c] = epoch after delivery) this is exactly
    \* "acked[c] = epoch at delivery"; a reset_acknowledgement() (acked := 0)
    \* is the explicit per-consumer re-arm that makes the redelivery legal,
    \* so it is NOT a duplicate.
    /\ dupDelivered' =
         IF DeliverNow(c) /\ acked[c] = epoch
           THEN TRUE
           ELSE dupDelivered
    \* Explicit per-consumer re-arm witness: a consumer re-delivers an epoch
    \* it ALREADY delivered (epoch \in deliveredEpochs[c]) with acked = 0 -
    \* i.e. only after reset_acknowledgement() re-armed it.
    /\ sawResetRedelivery' =
         IF DeliverNow(c) /\ epoch \in deliveredEpochs[c] /\ acked[c] = 0
           THEN TRUE
           ELSE sawResetRedelivery
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
    \* Per-consumer protection witness: the DELIVERING consumer c must have
    \* itself observed this epoch from a blocked cancel point (its own
    \* blockedCheckedEpochs[c]) - a different consumer's blocked observation
    \* cannot impersonate c's blocked -> unblocked -> deliver chain.
    /\ sawProtectedRequestDelivered' =
         IF DeliverNow(c) /\ epoch = blockedCheckedEpochs[c]
           THEN TRUE
           ELSE sawProtectedRequestDelivered
    \* Same-consumer rearm witness: c must have delivered the epoch that was
    \* re-armed (RearmedFromEpoch) BEFORE this delivery - a consumer's FIRST
    \* delivery after a rearm is a first delivery, not a re-delivery.
    /\ sawDeliverAfterRearm' =
         IF DeliverNow(c) /\ RearmedFromEpoch >= 1 /\ ~clearSinceRearm
              /\ epoch > RearmedFromEpoch
              /\ RearmedFromEpoch \in deliveredEpochs[c]
           THEN TRUE
           ELSE sawDeliverAfterRearm
    /\ UNCHANGED <<pending, epoch, postClear, requestedEpochs, blocked,
                   rearmEpoch, clearSinceRearm, lastClearedEpoch,
                   clearCount>>

Stutter == UNCHANGED vars

Next ==
    \/ Request
    \/ Rearm
    \/ Clear
    \/ \E c \in Consumers :
         \/ Protect(c)
         \/ Unprotect(c)
         \/ ResetAcknowledgement(c)
         \/ CancelPoint(c)
    \/ Stutter

Spec == Init /\ [][Next]_vars

(* ---- As-built safety laws (positive cfg, all PASS) ---- *)

(* Zig Io.zig:1186 single-shot per request, stated WITH the explicit re-arm
   authorities (review fix): within one request epoch each consumer delivers
   at most once UNLESS an explicit re-arm occurred - token-side rearm() (a
   new epoch) or the per-consumer CancelState::reset_acknowledgement()
   (acked := 0, ADR-cancel-request-epoch semantics table). dupDelivered is
   the history witness: a delivery with acked[c] = epoch is a duplicate
   WITHOUT such an authority. *)
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

(* CR3: token reuse - a consumer acked a request, then a NEW request
   generation (epoch >= 2) opened while pending (reached via rearm() or via
   clear()+request()). *)
NoReachReuse ==
    ~((epoch >= 2) /\ pending /\ (\E c \in Consumers : acked[c] >= 1))

(* CR5: the ADR fix is non-vacuous - the SAME consumer that delivered an
   effectively-cleared request epoch (lastClearedEpoch, recorded only by a
   clear() that drops a pending request) later delivers the FRESH request
   that replaced it. As-built, clear() leaves the epoch unchanged and only
   request() from idle advances the epoch after a clear, so the fresh
   request's identity is exactly lastClearedEpoch + 1; the witness therefore
   pins on (lastClearedEpoch + 1) \in deliveredEpochs[c] (PR #181 round-3
   tightening - "some strictly newer fe" alone still admitted a clear ->
   request -> rearm -> deliver-later-epoch chain in which the fresh request
   was never delivered). A rearm() path can never impersonate this witness:
   rearm() requires pending, so it cannot be the first advance after a clear,
   and it never records a cleared epoch. *)
NoReachNewRequestDelivered ==
    ~(\E c \in Consumers :
        /\ lastClearedEpoch >= 1
        /\ lastClearedEpoch \in deliveredEpochs[c]
        /\ (lastClearedEpoch + 1) \in deliveredEpochs[c])

(* Shared-token semantics (Group): one request delivers to BOTH consumers -
   per-consumer acknowledgement is real, not a single global bit. *)
NoReachSharedDelivered ==
    ~(\E c1 \in Consumers : \E c2 \in Consumers :
         c1 # c2 /\ pending /\ epoch \in deliveredEpochs[c1]
         /\ epoch \in deliveredEpochs[c2])

(* Protection-blocks-delivery-not-request: the SAME consumer that observed a
   request from a blocked cancel point later delivers it after unblocking
   (the request survived; per-consumer blockedCheckedEpochs[c] pins the
   same-consumer chain - no cross-consumer impersonation). *)
NoReachProtectedRequestDelivered == ~sawProtectedRequestDelivered

(* rearm() re-delivery (Zig Io.recancel): a consumer that already delivered a
   request delivers once more after rearm() - the request stays pending and
   its epoch advances. RearmedFromEpoch \in deliveredEpochs[c] pins that c
   delivered the re-armed epoch BEFORE this delivery. *)
NoReachRearmRedelivers == ~sawDeliverAfterRearm

(* reset_acknowledgement() re-delivery (ADR semantics table,
   T-CANCEL-SHARED-4): a consumer that already delivered the current request
   delivers it AGAIN after the explicit per-consumer re-arm - the reachable
   positive shape of "single-shot between explicit re-arms". *)
NoReachResetRedelivers == ~sawResetRedelivery

(* CR6: clean reuse start - after clear(), the token is genuinely idle
   (pending=0) before the next request. *)
NoReachClearedIdle == ~(pending = FALSE /\ clearCount >= 1 /\ epoch >= 1)
====
