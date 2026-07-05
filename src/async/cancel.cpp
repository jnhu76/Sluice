// Implementation of cooperative cancellation primitives (sluice-CORE-027, T1).
//
// See include/sluice/async/cancel.hpp for the model and Zig provenance. This
// file holds the non-trivial method bodies; the small inline ops (CancelGuard)
// stay in the header for RAII inlining.
#include <sluice/async/cancel.hpp>

#include <sluice/error.hpp>
#include <sluice/result.hpp>

namespace sluice::async {

void CancelToken::request() noexcept {
    // Release: pairs with is_requested()'s acquire so a consumer on another
    // thread that reads 'true' has a happens-before edge to this write (CP.20).
    requested_.store(1, std::memory_order::release);
}

bool CancelToken::is_requested() const noexcept {
    return requested_.load(std::memory_order::acquire) != 0;
}

void CancelToken::rearm() noexcept {
    // Re-arm only matters if the request was acknowledged (cleared). A request
    // already pending with no acknowledgement is unaffected. Either way the
    // token's "requested" bit is set; the per-consumer CancelState tracks
    // acknowledgement independently.
    requested_.store(1, std::memory_order::release);
}

void CancelToken::clear() noexcept {
    requested_.store(0, std::memory_order::release);
}

CancelProtection CancelState::swap_protection(CancelProtection next) noexcept {
    const CancelProtection prev = protection_;
    protection_ = next;
    return prev;
}

// The single cancel point. Three conditions to DELIVER (Zig Io.zig:1183-1188):
//   (a) token has a pending request,
//   (b) the consumer is unblocked (CancelProtection not blocked),
//   (c) the request has not already been acknowledged by this consumer.
// On delivery, mark acknowledged (single-shot). On non-delivery, leave state
// untouched so a later unblock/rearm can still deliver.
Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept {
    if (state.protection() == CancelProtection::blocked) {
        return {};  // protected region: suppress delivery (request stays pending)
    }
    if (state.acknowledged()) {
        return {};  // already delivered; single-shot (Zig Io.zig:1186)
    }
    if (!token.is_requested()) {
        return {};  // no request pending
    }
    // Deliver exactly once: acknowledge so the next point does not re-signal.
    state.acknowledge();
    return make_unexpected<void>(IoError{IoError::Code::canceled});
}

}  // namespace sluice::async
