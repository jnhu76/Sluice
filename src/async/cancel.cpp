// Implementation of cooperative cancellation primitives (sluice-CORE-027, T1).
//
// See include/sluice/async/cancel.hpp for the model, Zig provenance, and the
// ADR-cancel-request-epoch request-epoch representation. This file holds the
// non-trivial method bodies; the small inline ops (CancelGuard, CancelState
// queries) stay in the header for RAII inlining.
#include <sluice/async/cancel.hpp>

#include <sluice/error.hpp>
#include <sluice/result.hpp>

namespace sluice::async {

namespace {
// State layout (ADR-cancel-request-epoch): bit 0 is the request-pending bit;
// bits 1..63 are the request epoch. Adding kEpochInc to a word with bit 0
// clear advances the epoch by one without touching the pending bit.
constexpr std::uint64_t kPendingBit = 1;
constexpr std::uint64_t kEpochInc = 2;
}  // namespace

void CancelToken::request() noexcept {
    auto cur = state_.load(std::memory_order::relaxed);
    while (true) {
        if ((cur & kPendingBit) != 0) {
            return;  // idempotent: already pending, no re-arm (Zig cancelAwaitable)
        }
        const auto next = (cur + kEpochInc) | kPendingBit;  // epoch + 1, pending
        // Release: pairs with is_requested()/epoch()/check_cancel's acquire so
        // a consumer that observes the request has a happens-before edge to
        // this write (CP.20).
        if (state_.compare_exchange_weak(cur, next, std::memory_order::release,
                                         std::memory_order::relaxed)) {
            return;
        }
    }
}

bool CancelToken::is_requested() const noexcept {
    return (state_.load(std::memory_order::acquire) & kPendingBit) != 0;
}

std::uint64_t CancelToken::epoch() const noexcept {
    return state_.load(std::memory_order::acquire) >> 1;
}

void CancelToken::rearm() noexcept {
    auto cur = state_.load(std::memory_order::relaxed);
    while (true) {
        if ((cur & kPendingBit) == 0) {
            return;  // nothing pending to re-arm (idempotent no-op)
        }
        // Re-arm the SAME request: pending stays set, the epoch advances so
        // every consumer whose last delivery predates the new epoch delivers
        // once more at its next cancel point (Zig Io.recancel, Io.zig:1310).
        const auto next = (cur + kEpochInc) | kPendingBit;
        if (state_.compare_exchange_weak(cur, next, std::memory_order::release,
                                         std::memory_order::relaxed)) {
            return;
        }
    }
}

void CancelToken::clear() noexcept {
    // Clear the pending bit; the epoch is unchanged (a later request() creates
    // a fresh request by advancing it).
    state_.fetch_and(~kPendingBit, std::memory_order::release);
}

CancelProtection CancelState::swap_protection(CancelProtection next) noexcept {
    const CancelProtection prev = protection_;
    protection_ = next;
    return prev;
}

bool CancelState::acknowledged(const CancelToken& token) const noexcept {
    // "Acknowledged" is relative to the request currently pending on `token`:
    // the consumer delivered iff its last-acknowledged epoch matches the
    // token's current epoch. Best-effort introspection; check_cancel is the
    // authoritative single-shot enforcement.
    return token.is_requested() && acknowledged_epoch_ == token.epoch();
}

void CancelState::acknowledge(const CancelToken& token) noexcept {
    acknowledged_epoch_ = token.epoch();
}

// The single cancel point. Three conditions to DELIVER (Zig Io.zig:1183-1188):
//   (a) token has a pending request,
//   (b) the consumer is unblocked (CancelProtection not blocked),
//   (c) the request has not already been acknowledged by this consumer.
// On delivery, acknowledge the CURRENT request epoch (single-shot per request).
// On non-delivery, leave state untouched so a later unblock/rearm can still
// deliver.
Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept {
    if (state.protection() == CancelProtection::blocked) {
        return {};  // protected region: suppress delivery (request stays pending)
    }
    // One atomic snapshot (pending + epoch): the decision linearizes at a
    // single moment. A concurrent clear() is only observed by later checks; a
    // concurrent rearm() mid-check is seen as one consistent state.
    const auto word = token.state_.load(std::memory_order::acquire);
    if ((word & kPendingBit) == 0) {
        return {};  // no request pending
    }
    const auto epoch = word >> 1;
    if (state.acknowledged_epoch_ == epoch) {
        return {};  // already delivered this request; single-shot (Zig Io.zig:1186)
    }
    // Deliver exactly once per request: acknowledge the epoch so the next
    // point does not re-signal until rearm()/reset_acknowledgement().
    state.acknowledged_epoch_ = epoch;
    return make_unexpected<void>(IoError{IoError::Code::canceled});
}

}  // namespace sluice::async
