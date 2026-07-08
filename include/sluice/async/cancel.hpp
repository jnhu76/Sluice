// sluice::async cooperative cancellation primitives (sluice-CORE-027, T1).
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
// shares a token across its tasks). Thread-safe: request() may be called from
// any thread; consumers observe via is_requested() under acquire ordering.
class CancelToken {
public:
    CancelToken() = default;

    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;
    CancelToken(CancelToken&&) = delete;
    CancelToken& operator=(CancelToken&&) = delete;

    // Request cancellation. Idempotent: calling more than once is a no-op.
    // Memory ordering: release, so a subsequent is_requested() (acquire) on any
    // thread observes the request. Mirrors Zig Future.cancel idempotency.
    void request() noexcept;

    // Observe whether cancellation has been requested. Acquire ordering: pairs
    // with request()'s release. A cancel POINT calls this; the per-consumer
    // CancelState decides whether to DELIVER (protection) and whether to
    // ACKNOWLEDGE (single-shot).
    bool is_requested() const noexcept;

    // Re-arm a previously-acknowledged request so the next cancel point
    // delivers again. Mirrors Zig Io.recancel (Io.zig:1310). Used when a
    // consumer must report partial progress before re-propagating the cancel
    // (Zig Queue's pattern, Io.zig:2029). Idempotent.
    void rearm() noexcept;

    // Clear any pending request (used by cancellers that own the token and want
    // to reset it for reuse). Acks + clears. Idempotent.
    void clear() noexcept;

private:
    // bit 0: cancel requested. Other bits reserved. Plain atomic<bool> would
    // suffice; a uint8 leaves room for future state without ABI churn.
    std::atomic<std::uint8_t> requested_{0};
};

// Per-consumer cancellation state: the protection bit (delivery-blocking) and
// the acknowledgement bit (single-shot delivery). ONE of these lives inside
// each consumer (a task, a Future, a thread driving a loop). Not thread-safe
// in general — the consumer drives its own state — EXCEPT swap_protection,
// which the consumer may call from its own context (documented).
class CancelState {
public:
    // The current protection level. unblocked by default (Zig: tasks are
    // created unblocked, Io.zig:1325).
    CancelProtection protection() const noexcept { return protection_; }

    // Swap the protection level; returns the previous. RAII guard below is the
    // safe wrapper. Mirrors Zig Io.swapCancelProtection (Io.zig:1342).
    CancelProtection swap_protection(CancelProtection next) noexcept;

    // Has the most recent cancel request already been acknowledged (delivered
    // to a cancel point)? Used to enforce single-shot delivery.
    bool acknowledged() const noexcept { return acknowledged_; }

    // Mark the current request as acknowledged (a cancel point delivered it).
    // Subsequent cancel points will NOT re-deliver until rearm() on the token.
    void acknowledge() noexcept { acknowledged_ = true; }

    // Reset acknowledgement (e.g. when arming a fresh wait). Idempotent.
    void reset_acknowledgement() noexcept { acknowledged_ = false; }

private:
    CancelProtection protection_{CancelProtection::unblocked};
    bool acknowledged_{false};
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
// acknowledged. On returning canceled, the request is marked acknowledged
// (single-shot). Otherwise returns a successful Result<void> (no error).
//
// This is the canonical shape every cooperative cancel point implements; it is
// exposed publicly so long CPU-bound loops can call it directly (Zig's stated
// use case for checkCancel).
Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept;

}  // namespace sluice::async
