# CancelToken Request-Epoch Isolation — cooperative task cancellation (MODEL-007c)

Focused TLA+ safety model of the AS-BUILT `CancelToken` / `CancelState` /
`check_cancel` request-epoch protocol (ADR-cancel-request-epoch, Accepted
2026-08-13): a shareable cancel-request token whose identity is a monotonic
request EPOCH, with per-consumer single-shot acknowledgement **between the two
explicit re-arm authorities**: token-side `rearm()` and per-consumer
`reset_acknowledgement()`.

Issue #180 (child of umbrella #171; the last residual of audit #162
MODEL-007). C++ is the fact source — the model conforms to the
implementation, never the reverse.

## C++-first recovery: this is a REQUEST-epoch protocol, not a task-incarnation protocol

The original MODEL-007(c) brief hypothesized a task-incarnation epoch (a
reusable task object, per-incarnation tokens, "old token cancels new
incarnation"). The C++ recovery (issue #180 Comment A) showed the as-built
protocol is different and the model boundary follows the C++:

- `CancelToken` is a **long-lived shareable** object (a `Group` shares ONE
  token across its tasks — `group.hpp`); there is no per-incarnation token
  issuance.
- The epoch is the identity of the **request**, not of a task incarnation. It
  advances on `request()` from idle and on `rearm()`, and is read by every
  consumer's acknowledgement comparison.
- "Reuse" is **token reuse** (`clear()` + a new `request()` is a NEW request
  that re-delivers to previously-acked consumers) and **per-consumer
  acknowledgement across request generations** (`CancelState::acked_epoch`).
- The load-bearing questions therefore become:
  1. can a stale ack (from an old request) **starve** delivery of a newer
     request? (pre-fix defect, ADR §1 finding 2);
  2. can a cleared request's cancel intent **survive** reuse? (the
     `clear()`-keeps-pending defect, HC3);
  3. can a delivery happen **without** a pending request (ghost delivery)?
  4. is single-shot-per-request real, and is protection really
     blocks-delivery-not-request?

Hypotheses HC1–HC4 from the brief mapped to the as-built protocol:

| brief hypothesis | as-built disposition |
|---|---|
| HC1 "no epoch check → stale token mutates current" | **KEPT** as the sticky-ack representation (pre-fix `acknowledged_` bool: the ack is not request-relative, so a reused token is permanently dead for a previously-acked consumer) → NEG-CT1 |
| HC2 "epoch advanced too late" | **DISCARDED with reason**: `request()`/`rearm()` set the pending bit and advance the epoch in ONE CAS (`cancel.cpp:28,56`); there is no observable window where a new request is visible with an old epoch |
| HC3 "cancel intent survives reuse" | **KEPT** as `clear()`-keeps-pending (a cleared token would still deliver the old request to the next cancel point) → NEG-CT2 |
| HC4 "current-epoch token cancels after terminal boundary" | **DISCARDED with reason**: the token protocol has no task terminal boundary; the analog (idle `rearm()`) is a documented no-op and produces no defect |

Two further real one-rule defects are modeled (the prompt's "pick a real
one-rule defect" for the dropped HC2): dropping the single-shot ack gate
(NEG-CT3, Zig's single-shot contract, ADR §2.1) and dropping the pending gate
(NEG-CT4, the provenance/anti-ghost law). Protection is covered by NEG-CT5.

## C++ binding

| Model construct | C++ (baseline e035ff5) |
|---|---|
| `pending`, `epoch` (one token) | `CancelToken::state_` — one atomic uint64: bit 0 pending, bits 1..63 request epoch (`cancel.hpp:111`, `cancel.cpp:16-19`) |
| `Request` (idle → pending, epoch+1, one CAS) | `CancelToken::request()` (`cancel.cpp:22-37`) — `(cur + kEpochInc) \| kPendingBit` in one compare-exchange; idempotent no-op while pending |
| `Rearm` (pending → epoch+1, one CAS) | `CancelToken::rearm()` (`cancel.cpp:47-62`) — Zig `Io.recancel`; idle rearm is a no-op |
| `Clear` (pending → 0, epoch unchanged) | `CancelToken::clear()` (`cancel.cpp:64-68`) — `fetch_and(~kPendingBit)` |
| `CancelPoint(c)` (one atomic snapshot) | `check_cancel(token, state)` (`cancel.cpp:95-114`) — one acquire load of pending+epoch, so a delivery linearizes at a single moment; a concurrent `clear()` only affects later checks |
| `ResetAcknowledgement(c)` (`acked[c] := 0`) | `CancelState::reset_acknowledgement()` (`cancel.hpp:151`) — the explicit per-consumer re-arm: the next cancel point delivers the CURRENT request again (ADR semantics table; `tests/cancel_token_test.cpp` T-CANCEL-SHARED-4) |
| `acked[c]` | `CancelState::acknowledged_epoch_` (per-consumer last-delivered epoch) |
| `blocked[c]` / `Protect` / `Unprotect` | `CancelState::protection_` + `swap_protection` / `CancelGuard` |
| `DeliverNow` = unblocked ∧ pending ∧ `acked[c] # epoch` | the three delivery conditions (Zig Io.zig:1183-1188): (a) pending, (b) unblocked, (c) not already acknowledged |
| `postClear` | the observable "a cleared token is idle" phase (clear sets it; the next request clears it) |
| shared token, 2 consumers | Group semantics (`group.hpp`): one token, per-task `CancelState`; each consumer delivers once per request (`tests/cancel_token_test.cpp` T-CANCEL-SHARED-4) |

Memory-model boundary: `state_` is one atomic uint64 (release CAS / release
fetch_and / acquire load); the per-consumer `CancelState` fields are plain and
consumer-owned. TLC proves the SC protocol abstraction only — **no C++ weak
memory proof**; the README makes no claim about relaxed/acquire-release
ordering subtleties beyond the single-snapshot linearization that the model
mirrors exactly.

## Boundary

1 token, 2 consumers {A, B}, 1 canceller, request generations `0..MaxEpoch`
(= 3, enough for the NEG-CT1 trace: request, rearm, rearm, first cancel
point), `0..MaxClear` (= 2). Safety only.

`CancelState::reset_acknowledgement()` (the per-consumer re-arm) **IS modeled**
(review fix, PR #181 round 2): the as-built `CancelState` carries an explicit
per-consumer re-arm authority (`cancel.hpp:151`, ADR semantics table) that
deliberately re-opens the SAME request epoch to a second delivery for that one
consumer. The single-shot law is therefore stated as "no duplicate delivery
**without** an explicit re-arm authority" — token-side `rearm()` (a new
epoch) or per-consumer `reset_acknowledgement()` (`acked := 0`) — not as an
absolute per-epoch single-shot.

Out of scope (explicit non-goals):

- `acknowledged()` / `acknowledge()` introspection — best-effort
  test-facing helpers; `acknowledge()` has no production caller outside
  `check_cancel` (ADR §2.3).
- The Future/Group task machinery, the Scheduler, wait registration, and
  backend op cancel (ADR X2/X3, `tests/async_cancel_test.cpp`) — disjoint
  layers (AC-9); the token is the T1 authority.
- Timer/deadline admission (e11's domain) and the task-cancel layer of the
  runtime (e16's domain).

## Laws (positive cfg, all PASS)

| invariant | meaning |
|---|---|
| `InvSingleShotPerEpoch` | within one request epoch each consumer delivers at most once **between explicit re-arm authorities** (review fix): `dupDelivered` is a delivery with `acked[c] = epoch`, i.e. a duplicate WITHOUT such an authority — token-side `rearm()` (a new epoch) or per-consumer `reset_acknowledgement()` (`acked := 0`) make the redelivery legal, so it is not a duplicate |
| `InvNoDeliveryWhenIdle` | no delivery without a pending request — the provenance/anti-ghost law |
| `InvProtectionBlocksDelivery` | a blocked cancel point never delivers; the request stays pending (Zig CancelProtection) |
| `InvClearRemovesIntent` | a cleared token is idle until the next request (`postClear => ~pending`) — cancel intent does not survive reuse |
| `InvNoStaleAckStarvesDelivery` | a consumer whose most recent **unblocked** cancel point ran under the current pending request has delivered it — the request-relative ack's observable consequence (protected checks deliberately advance no record: a protected region sees no cancel points) |
| `InvDeliveredWasRequested` | **historical provenance**: every delivered epoch was a real created request |
| `InvAckIsRealEpoch` | the ack is request-relative: it always equals a request epoch actually delivered (or 0) — the ADR's representation law |

## Negative controls (one-rule cfg flips; each names its invariant)

| gate | defect | named CEX | specificity exclusions |
|---|---|---|---|
| NegStickyAck | **pre-fix representation**: the ack is a sticky bool, never request-relative (`acknowledged_` bare bool, ADR §1 finding 2) | `InvAckIsRealEpoch` (a first delivery at epoch ≠ 1 leaves `acked = 1` pointing at no delivered request) | co-victim `InvNoStaleAckStarvesDelivery` (the sticky ack starves delivery of every later request — the observable consequence of the same root defect). CEX trace: `Request → Rearm → CancelPoint` delivers epoch 2 with `acked = 1` |
| NegClearKeepsPending | `clear()` keeps the pending bit (HC3: cancel intent survives reuse) | `InvClearRemovesIntent` | none — the stale delivery still occurs with `pending=1`, so the other laws hold. CEX trace: `Request → Clear` leaves `postClear ∧ pending` |
| NegDropSingleShot | the ack/epoch gate is dropped — every unblocked pending check delivers | `InvSingleShotPerEpoch` | none — the pending/protection gates and the epoch-relative ack update are intact. CEX: `Request → CancelPoint → CancelPoint` — the second delivery has `acked[c] = epoch` (a duplicate with NO re-arm authority) |
| NegDropPendingCheck | the pending gate is dropped — a check delivers with no pending request | `InvNoDeliveryWhenIdle` | none — an idle delivery still uses the current epoch (epoch 0 is never delivered: `acked` starts at 0), so the provenance law holds |
| NegDropProtection | the protection gate is dropped — a blocked consumer delivers | `InvProtectionBlocksDelivery` | none |

## Reachability (9 witnesses, each a NoReach* CEX)

`NoReachRequestCreated` (a request exists) · `NoReachDelivered` (a consumer
delivered) · `NoReachReuse` (a consumer acked, then a NEW request generation
≥ 2 opened) · `NoReachNewRequestDelivered` (the SAME consumer that delivered
an effectively-cleared request epoch — `lastClearedEpoch`, recorded only by a
`clear()` that drops a pending request — later delivers a strictly newer
epoch; only `request()` can advance the epoch while the token is idle after a
clear, so that newer delivery is exactly the clear+request reuse path, and the
`rearm()` path cannot impersonate it because rearm never records a cleared
epoch; review fix — replaces the weaker `Cardinality ≥ 2` pin) ·
`NoReachSharedDelivered` (one request delivered to BOTH consumers — Group
per-consumer ack is real) · `NoReachProtectedRequestDelivered` (the SAME
consumer that observed a request from a blocked cancel point later delivers it
after unblocking — per-consumer `blockedCheckedEpochs[c]`; a different
consumer's blocked observation cannot impersonate the chain; review fix) ·
`NoReachRearmRedelivers` (the SAME consumer that delivered the re-armed epoch
delivers again after `rearm()` with no intervening clear — `RearmedFromEpoch ∈
deliveredEpochs[c]`; a consumer's FIRST delivery after a rearm is not a
re-delivery; review fix) · `NoReachResetRedelivers` (a consumer that already
delivered the CURRENT request delivers it AGAIN after the explicit
per-consumer `reset_acknowledgement()` — the reachable positive shape of
"single-shot between explicit re-arms"; new witness) · `NoReachClearedIdle`
(after clear the token is genuinely idle before the next request).

## Adversarial probes (temp workspace, not committed)

- **Probe A** (remove the epoch/ack gate) = NEG-CT3: exact `InvSingleShotPerEpoch` CEX; removing the ack-epoch relation (sticky) = NEG-CT1: exact `InvAckIsRealEpoch` CEX. Both one-rule, both name-asserted.
- **Probe B** (weaken the reach witness, review-fix): the ORIGINAL `NoReachNewRequestDelivered` form (`acked = epoch ∧ epoch ≥ 2`) was satisfiable by a FRESH consumer (rearm before its first check makes `acked` jump 0→2), and even the `Cardinality ≥ 2` strengthening was impersonated by `Request → CancelPoint(A) [delivers 1] → Rearm → CancelPoint(A) [delivers 2]` — no `Clear` at all. The witness is now pinned on `lastClearedEpoch`: the SAME consumer must have delivered an effectively-cleared epoch and a strictly newer one. The CEX is the honest reuse path `Request → CancelPoint(A) [delivers 1] → Clear → Request → CancelPoint(A) [delivers 2]`; an INIT probe proves the rearm-only continuation (`Request → deliver → Rearm`, cancel-point-only next-state) holds the witness.
- **Probe C** (carry cancel across reset) = NEG-CT2: exact 3-step CEX (`Request → Clear`).
- **Probe D** (inject a compound/collateral defect): `CheckPending = FALSE ∧ ProtectionBlocks = FALSE` violates BOTH `InvNoDeliveryWhenIdle` and `InvProtectionBlocksDelivery` — the per-defect specificity gates catch collateral damage (reordering the cfg moves the violation to the other invariant).
- **Probe E** (remove the stale/reuse path): deleting `Clear` alone does NOT kill the reuse witnesses — `rearm()` is an alternative request-generation-advance path (the model's reuse semantic is "a consumer delivers across request generations", reachable via rearm or clear+request). Deleting BOTH `Rearm` and `Clear` makes `NoReachNewRequestDelivered` and `NoReachRearmRedelivers` PASS — the reach gates then fail closed ("expected CEX, model passed").
- **Probe F** (change the expected named invariant): running NegStickyAck against the wrong name `InvSingleShotPerEpoch` is rejected by the verifier's named-violation grep; the correct name `InvAckIsRealEpoch` is accepted.
- **Probe G** (protection cross-consumer impersonation, review-fix): `Request → Protect(A) → CancelPoint(A) [blocked, observes epoch 1] → CancelPoint(B) [delivers epoch 1]` no longer lights `sawProtectedRequestDelivered` — the witness requires the DELIVERING consumer's own `blockedCheckedEpochs[c]` (per-consumer history). An INIT probe proves the fake continuation holds the witness; the honest chain is `Request → Protect(A) → CancelPoint(A) → Unprotect(A) → CancelPoint(A) [A delivers]`.
- **Probe H** (rearm first-delivery, review-fix): `Request → Rearm → B first CancelPoint` no longer lights `sawDeliverAfterRearm` — `RearmedFromEpoch ∈ deliveredEpochs[c]` requires B to have delivered the re-armed epoch first. An INIT probe proves the fake continuation holds.
- **Probe I** (reset-authorized redelivery, review-fix): a consumer that delivered the CURRENT request and then called `reset_acknowledgement()` re-delivers it LEGALLY — an INIT probe proves `InvSingleShotPerEpoch` holds through the redelivery (the restated "single-shot between explicit re-arms" law), while `ReachResetRedelivers` witnesses the positive shape.

## Results

TLC 2.19 (tla2tools 1.7.4), exhaustive, 1 worker:

- positive: 9,322,345 states generated, 985,068 distinct, depth 27, no error
  (the review-fix history ghosts — per-consumer `blockedCheckedEpochs` scalar,
  `lastClearedEpoch`, `sawResetRedelivery`, and the `ResetAcknowledgement`
  action — grew the state space from the original 67,084 distinct; the
  full gate stays ~2 min).
- all 5 negative gates: exact named CEX; all 5 specificity gates PASS; all 9
  reachability gates CEX as expected (each an honest witness trace).
- `bash scripts/formal/verify-cancel-token-epoch.sh` → 20/20, PASS.
- adversarial probes A–I all behave as documented above (review-fix probes
  B/G/H/I prove the impersonation continuations are excluded and the
  reset-authorized redelivery is legal).

## C++ bridges (no new tests; existing deterministic coverage)

- `tests/cancel_token_test.cpp` — the direct T1 bridge, all deterministic
  (pure-logic, single-threaded; the one thread-based case T-CANCEL-FUTURE-5
  handshakes on the token's own atomic with no sleeps):
  - `cancel_request_then_check_delivers_canceled` (request → deliver)
  - `cancel_single_shot_second_check_does_not_resignal` (single-shot)
  - `cancel_rearm_re_enables_delivery` (rearm re-delivery — **fails on the
    pre-fix code**, GREEN after ADR; the NEG-CT1/`NoReachRearmRedelivers`
    bridge)
  - `cancel_protection_blocks_delivery_not_request` and
    `cancel_protection_rearm_blocks_until_unprotected_point` (protection
    blocks delivery, not the request — the `InvProtectionBlocksDelivery` /
    `NoReachProtectedRequestDelivered` bridge)
  - `cancel_clear_then_request_is_a_fresh_request` (clear + new request
    re-delivers — **fails on the pre-fix code**; the NEG-CT1 /
    `NoReachNewRequestDelivered` bridge)
  - `cancel_shared_token_two_consumers_deliver_and_rearm` (per-consumer ack,
    shared token — the `NoReachSharedDelivered` bridge; it also executes
    `b.reset_acknowledgement()` and asserts only B re-delivers the same
    request — the `NoReachResetRedelivers` / restated `InvSingleShotPerEpoch`
    bridge, review-fix)
  - `cancel_request_idempotent_clear_resets` (idempotent request, clear)
- `tests/async_cancel_test.cpp` — backend op cancellation (ADR X2/X3);
  adjacent, out of the token protocol's scope.
- `tests/request_arena_cancel_intent_test.cpp` — the arena's causal stale
  cancel (NEG-RA-2); adjacent (RequestArena domain, AC-14), not the token.

The model's negative gates correspond to regressions that the two ADR tests
(`cancel_rearm_re_enables_delivery`, `cancel_clear_then_request_is_a_fresh_
request`) assert fail on the pre-fix implementation (ADR §4: "The rearm and
clear+request cases fail on the pre-fix implementation (RED) and pass after
(GREEN)"). No production seam was needed and no production code was touched.

## Verdict

**AS-BUILT MODELED.** No C++ defect candidate. Allowed claims: the SC
abstraction of the request-epoch protocol satisfies the seven laws above
(including the restated single-shot law with its two explicit re-arm
authorities — token `rearm()` and per-consumer `reset_acknowledgement()` — a
redelivery under either authority is legal, matching the ADR semantics
table), and each of the five studied single-rule defects (sticky ack /
clear-keeps-pending / dropped single-shot / dropped pending gate / dropped
protection) is excluded by the as-built protocol. The historical pre-fix
defect (ADR §1 finding 2) is exactly reproduced by NEG-CT1 and shown to be
excluded by the epoch-relative ack. Forbidden claims: C++ weak-memory
correctness, implementation freedom from bugs, and "TLC green ⇒ C++ correct".
