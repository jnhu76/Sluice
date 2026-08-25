// sluice::async cooperative cancellation primitives (T1).
//
// Derived from Zig std.Io's cancellation model (Io.zig:1183-1188, 1310-1358):
//
//   - Cancelable = error{Canceled} (one error). cppio reuses IoError::Code::canceled.
//   - Single-shot per cancellation point: only the NEXT cancelation point in a
//     consumer returns Canceled; subsequent points do not re-signal unless the
//     consumer calls recancel (Zig Io.recancel, Io.zig:1310).
//   - CancelProtection blocks DELIVERY (not the request): a protected region
//     observes no cancellation points (Zig Io.CancelProtection, Io.zig:1322).
//
// Request identity (ADR-cancel-request-epoch, 2026-08-13): the token carries a
// monotonic request EPOCH alongside the pending bit. Per-consumer
// acknowledgement records the last delivered epoch, so "acknowledged" is
// always relative to a specific request: rearm() advances the epoch and every
// consumer that already delivered the previous epoch delivers once more at its
// next cancel point (the shared-token generalization of Zig's per-task
// .canceling/.canceled/recancel status machine, Io/Threaded.zig). The pre-fix
// representation (a bare acknowledgement bool) made rearm() a no-op and made
// clear()+request() undeliverable to previously-acked consumers.
//
// This is the cooperative layer the task runtime (T2 Future / T3 Group) wraps.
// It is deliberately free of any scheduler/fiber/thread-pool dependency: a
// CancelToken is an atomic state, and the per-consumer protection/acknowledge
// state is owned by whoever drives the consumer (a task, a Future, a thread).
//
// Layering: lives ABOVE AsyncBackend (the op-execution seam). It does not touch
// AsyncBackend; backends keep their own best-effort op cancel (ADR §7 X2). The
// relationship is: a task (T2/T3) owns a CancelState; on entering an Io-style
// cancel point it calls check_cancel(token, state); if delivered, the task
// propagates IoError::canceled up to its driver, which may then cancel the
// backend op via the existing AsyncIoContext::cancel.
#pragma once

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstdint>

namespace sluice::async {

class CancelState;  // defined below; needed by CancelToken's check_cancel friend

// Delivery-blocking protection level. Mirrors Zig Io.CancelProtection
// (Io.zig:1322): `blocked` makes no cancel point deliver — it blocks DELIVERY
// of an already-requested cancel, not the request itself. The default for every
// consumer is `unblocked` (Zig: tasks are created unblocked, Io.zig:1325).
enum class CancelProtection : std::uint8_t {
    unblocked = 0,
    blocked = 1,
};

// The cooperative cancel-request state, shareable between cancellers and one or
// more consumers. Mirrors Zig's per-task "cancel requested" bit but decoupled
// from the task object so cppio can compose it (a Future wraps a token; a Group
// shares a token across its tasks). Thread-safe: request()/rearm()/clear() may
// be called from any thread; consumers observe via is_requested()/epoch() under
// acquire ordering.
//
// State layout: one atomic uint64; bit 0 is the request-pending bit, bits 1..63
// are the request epoch (generation). The epoch is the identity of the current
// request: it advances on every request() that transitions idle -> pending and
// on every rearm(). 63-bit monotonic-while-pending; wrap-around is unreachable.
class CancelToken {
public:
    CancelToken() = default;

    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;
    CancelToken(CancelToken&&) = delete;
    CancelToken& operator=(CancelToken&&) = delete;

    // Request cancellation. Idempotent: while a request is already pending,
    // calling again is a no-op and does NOT re-arm delivery (use rearm()).
    // Memory ordering: release, so a subsequent is_requested() (acquire) on any
    // thread observes the request. Mirrors Zig Future.cancel idempotency and
    // cancelAwaitable's no-op on .canceling/.canceled (Io/Threaded.zig).
    void request() noexcept;

    // Observe whether cancellation is pending. Acquire ordering: pairs with
    // request()/rearm()'s release. A cancel POINT calls this; the per-consumer
    // CancelState decides whether to DELIVER (protection) and whether to
    // ACKNOWLEDGE (single-shot).
    bool is_requested() const noexcept;

    // The current request epoch (generation): the identity of the request
    // pending on this token. Consumers compare their last-acknowledged epoch
    // against it (CancelState::acknowledged, check_cancel). Monotonic while
    // pending. Acquire ordering.
    std::uint64_t epoch() const noexcept;

    // Re-arm a previously-acknowledged request so the next cancel point
    // delivers again. Mirrors Zig Io.recancel (Io.zig:1310) generalized to the
    // shared-token case: the request stays pending, its epoch advances, and
    // every consumer whose last delivery predates the new epoch delivers once
    // more at its next cancel point. Used when a consumer must report partial
    // progress before re-propagating the cancel (Zig Queue's pattern,
    // Io.zig:2029). Idempotent; a no-op when no request is pending.
    void rearm() noexcept;

    // Clear any pending request (used by cancellers that own the token and want
    // to reset it for reuse). No cancel point delivers until the next
    // request(); that next request() is a NEW request and delivers again to
    // every consumer (the epoch advances). Per-consumer acknowledgement state
    // is untouched. Idempotent.
    void clear() noexcept;

private:
    // bit 0: cancel requested. Bits 1..63: request epoch.
    std::atomic<std::uint64_t> state_{0};

    // check_cancel reads ONE atomic snapshot (pending + epoch) so a delivery
    // decision linearizes at a single moment.
    friend Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept;
};

// Per-consumer cancellation state: the protection bit (delivery-blocking) and
// the acknowledgement epoch (single-shot delivery per request). ONE of these
// lives inside each consumer (a task, a Future, a thread driving a loop). Not
// thread-safe in general — the consumer drives its own state — EXCEPT
// swap_protection, which the consumer may call from its own context
// (documented).
class CancelState {
public:
    // The current protection level. unblocked by default (Zig: tasks are
    // created unblocked, Io.zig:1325).
    CancelProtection protection() const noexcept { return protection_; }

    // Swap the protection level; returns the previous. RAII guard below is the
    // safe wrapper. Mirrors Zig Io.swapCancelProtection (Io.zig:1342).
    CancelProtection swap_protection(CancelProtection next) noexcept;

    // Has the request currently pending on `token` already been acknowledged
    // (delivered to a cancel point) by THIS consumer? "Acknowledged" is only
    // meaningful relative to a specific request, hence the token parameter: a
    // bare bool cannot distinguish "acknowledged the current request" from
    // "acknowledged an older request". Introspection only — check_cancel is the
    // authoritative single-shot enforcement.
    bool acknowledged(const CancelToken& token) const noexcept;

    // Mark the request currently pending on `token` as acknowledged (a cancel
    // point delivered it). check_cancel is the canonical caller; exposed for
    // symmetry with acknowledged(). Subsequent cancel points will NOT re-deliver
    // until rearm() on the token or reset_acknowledgement() on this state.
    void acknowledge(const CancelToken& token) noexcept;

    // Per-consumer re-arm: the next cancel point delivers the current request
    // again, WITHOUT affecting other consumers sharing the token (token-side
    // rearm() re-arms the shared request for every consumer). Idempotent.
    void reset_acknowledgement() noexcept { acknowledged_epoch_ = 0; }

private:
    CancelProtection protection_{CancelProtection::unblocked};
    std::uint64_t acknowledged_epoch_{0};

    // check_cancel records the delivered request epoch directly.
    friend Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept;
};

// RAII guard for a protected region. Mirrors Zig's documented usage
// (Io.zig:1334-1339):
//   { CancelGuard g{state, CancelProtection::blocked}; do_work(); }
// On construction swaps to `next`; on destruction restores the previous. The
// region observes no cancellation points. [[nodiscard]] so a caller cannot
// forget to bind it (which would make it a no-op that destructs immediately).
class [[nodiscard]] CancelGuard {
public:
    CancelGuard(CancelState& state, CancelProtection next) noexcept
        : state_(&state), prev_(state.swap_protection(next)) {}
    ~CancelGuard() {
        if (state_) (void)state_->swap_protection(prev_);
    }
    CancelGuard(const CancelGuard&) = delete;
    CancelGuard& operator=(const CancelGuard&) = delete;
    CancelGuard(CancelGuard&& other) noexcept
        : state_(other.state_), prev_(other.prev_) { other.state_ = nullptr; }
    CancelGuard& operator=(CancelGuard&&) = delete;  // simpler; not needed yet

private:
    CancelState* state_;
    CancelProtection prev_;
};

// A pure cancelation point (Zig Io.checkCancel, Io.zig:1356). Returns
// IoError::canceled if: (a) the token has a request pending, AND (b) the
// consumer's state is unblocked, AND (c) the request has not already been
// acknowledged by this consumer. On returning canceled, the request is marked
// acknowledged (single-shot per request epoch). Otherwise returns a successful
// Result<void> (no error).
//
// The check reads one atomic snapshot of the token (pending + epoch), so it
// linearizes at a single moment: a concurrent clear() only affects later
// checks, and a concurrent rearm() that lands mid-check is observed atomically.
//
// This is the canonical shape every cooperative cancel point implements; it is
// exposed publicly so long CPU-bound loops can call it directly (Zig's stated
// use case for checkCancel).
Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept;

}  // namespace sluice::async
