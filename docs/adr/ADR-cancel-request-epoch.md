# ADR: Cancel Request Epoch (rearm semantics)

```text
Status: Accepted
Date: 2026-08-13
Baseline SHA: 5e5ec3663d69f09b5b571ea01c9f4200c75aec98 (master, PR #93 merge)
Supersedes: the delivery-acknowledgement representation of the cooperative
            cancellation primitive described in ADR-execution-model
            ("single-shot cancel delivery" contract, unchanged in effect)
Superseded by: none
```

## 1. Context

The cooperative cancellation primitives (`CancelToken` / `CancelState` /
`check_cancel`, sluice-CORE-027 T1) document a Zig `std.Io`-derived contract:

- `request()` is an idempotent cancel request;
- the next unblocked, unacknowledged cancel point delivers `IoError::canceled`
  exactly once;
- `rearm()` mirrors Zig `Io.recancel` (Io.zig:1310): "re-arm a
  previously-acknowledged request so the next cancel point delivers again";
- `clear()` resets the token for reuse.

A post-Phase-D audit (issue #94 corrective pass) found the implementation does
not deliver the documented contract:

1. **`rearm()` is a no-op.** It re-stores `requested_ = 1` while the per-consumer
   acknowledgement lives in `CancelState::acknowledged_`. After a delivery,
   `check_cancel` keeps returning success because the acknowledgement bit never
   changes. The existing test papered over this by requiring the consumer to
   also call `CancelState::reset_acknowledgement()` after `rearm()` — the
   documented contract ("rearm alone re-enables delivery") was not encoded in
   any passing test.
2. **The acknowledgement bit is not request-relative.** `acknowledged_` is a
   bare bool; it cannot distinguish "acknowledged the *current* request" from
   "acknowledged an *older* request". Consequently `clear()` + a later
   `request()` never re-delivers to a consumer that acknowledged the earlier
   request — the token is permanently dead for that consumer.
3. **The token/state split is real and must stay.** `CancelToken` is shareable
   (Group shares one token across tasks; Future owns one token observed by a
   producer with its own `CancelState`). The acknowledgement is per-consumer
   by design: two consumers sharing one token must each deliver once. A fix
   that moves acknowledgement into the token would break shared-token
   semantics; a fix that makes `rearm()` reset one consumer's bit cannot, from
   the token side, know which consumer to reset.

Zig's own model (verified at pinned upstream `ziglang/zig` `89e0881f`,
`Io/Threaded.zig`): per-task status `.none → .canceling → .canceled`; `cancel()`
is a no-op on `.canceling`/`.canceled`; `checkCancel` delivers only from
`.canceling` and moves to `.canceled`; `recancel()` asserts `.canceled` and
flips back to `.canceling` (re-arms the *same* request); protection blocks
delivery, never the request. Zig has no shared-token case and no `clear()`; the
Sluice extensions must compose without contradicting this core.

## 2. Decision

Represent the cancellation request by an **epoch** (request generation) on the
token, and per-consumer acknowledgement as **the last delivered epoch**:

```text
CancelToken  : pending bit + monotonic request epoch  (one atomic uint64)
CancelState  : protection + last-acknowledged epoch    (consumer-owned)

request()  : pending==0 -> set pending, epoch += 1   (idempotent: no-op when pending)
rearm()    : pending==1 -> epoch += 1 (pending stays set)
             pending==0 -> no-op (nothing to re-arm)
clear()    : pending = 0 (epoch unchanged)

check_cancel(token, state):
    deliver iff state unblocked
             AND token pending
             AND state.acked_epoch != token.epoch
    on delivery: state.acked_epoch = token.epoch; return IoError::canceled
```

The epoch is the **request identity**: two consumers that acknowledge the same
epoch have delivered the same request; rearm() or a fresh request() advance the
identity so every consumer that already delivered re-delivers exactly once at
its next cancel point.

### 2.1 Semantics table

| Operation | Token state | Effect on delivery |
|-----------|-------------|--------------------|
| `request()` (pending==0) | pending=1, epoch++ | every consumer's next cancel point delivers once |
| `request()` (pending==1) | no-op | no re-delivery (idempotent; mirrors Zig `cancel` on `.canceling`) |
| `rearm()` (pending==1) | epoch++ | every consumer that already delivered re-delivers once (mirrors Zig `recancel`: re-arm the same request) |
| `rearm()` (pending==0) | no-op | nothing to re-arm (Zig asserts; Sluice documents idempotent) |
| `clear()` | pending=0 | no delivery until the next `request()` |
| `clear()` + `request()` | pending=1, epoch++ | a *new* request; previously-acked consumers deliver once more (token reuse for real) |
| `CancelState::reset_acknowledgement()` | — | per-consumer re-arm: next cancel point delivers the current request again |

Per-consumer single-shot is retained: within one request epoch, each consumer
delivers at most once (`state.acked_epoch == token.epoch` blocks re-delivery).

### 2.2 Protection semantics (unchanged)

`CancelProtection::blocked` suppresses *delivery*, never the request. A
protected cancel point leaves both token and consumer state untouched, so an
unblocked point later delivers the same request.

### 2.3 API delta (public)

`CancelState::acknowledged()` and `CancelState::acknowledge()` gain a
`const CancelToken&` parameter, because "acknowledged" is only meaningful
relative to a specific request:

```cpp
bool acknowledged(const CancelToken& token) const noexcept;  // delivered the token's current request?
void acknowledge(const CancelToken& token) noexcept;         // record delivery of the token's current request
```

`check_cancel` is the only production caller of `acknowledge()`; no production
caller uses `acknowledged()` (only tests). `request` / `is_requested` / `rearm`
/ `clear` / `protection` / `swap_protection` / `reset_acknowledgement` /
`check_cancel` / `CancelGuard` signatures are unchanged.

## 3. Why this is correct

- **Shared token, per-consumer ack:** each consumer stores its own
  `acked_epoch`; a Group task never steals another task's acknowledgement.
- **No lost rearm:** rearm advances a monotonic epoch on the token; any
  consumer whose acked epoch differs delivers. A consumer preempted between
  reading the epoch and writing its acknowledgement can only record an
  *older* epoch than the current one, which re-delivers — never suppresses.
- **No ABA:** the epoch strictly increases while pending; 63-bit width makes
  wrap-around unreachable. `request()` does not advance the epoch when already
  pending, so repeated idempotent requests cannot silently re-deliver.
- **Linearization:** `check_cancel` reads the token state in one atomic
  snapshot (pending + epoch), so it linearizes at a moment when the request
  existed; a concurrent `clear()` only affects later checks.
- **Zig conformance:** the epoch model is the shared-token generalization of
  Zig's `.canceling`/`.canceled`/`recancel` status machine; the mechanism class
  remains faithful (token-side request status + per-consumer delivery state).

## 4. Verification

- `tests/cancel_token_test.cpp`: T-CANCEL-REARM-1 (re-delivery after rearm),
  T-CANCEL-PROTECTION-2, T-CANCEL-CLEAR-3, T-CANCEL-SHARED-4 (two consumers,
  one token), T-CANCEL-FUTURE-5 (public `Future<T>` consumer), plus the
  updated single-shot / protection slices. The rearm and clear+request cases
  fail on the pre-fix implementation (RED) and pass after (GREEN).
- `docs/api-reference.md` updated for the new `acknowledge`/`acknowledged`
  signatures.
- CancelProtection / recancel family reclassification (DIV-11) re-verified in
  `docs/architecture/divergence-registry.md` and
  `docs/architecture/zig-io-conformance-map.md`.
