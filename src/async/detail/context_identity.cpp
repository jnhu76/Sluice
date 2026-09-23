#include <sluice/async/detail/context_identity.hpp>

#include <sluice/async/detail/fail_fast.hpp>

#include <atomic>
#include <cstdint>
#include <limits>

namespace sluice::async::detail {

namespace {

// The cursor carries the whole domain state: 1..UINT64_MAX are handed out once
// each and 0 is the sticky exhausted marker. 0 never changes meaning: it is also
// the unadopted slot-table value, so it is never a live context identity.
std::atomic<std::uint64_t> g_context_identity_cursor{1};

}

std::uint64_t claim_context_identity(std::atomic<std::uint64_t>& cursor) noexcept {
    std::uint64_t current = cursor.load(std::memory_order_relaxed);
    for (;;) {
        if (current == kContextIdentityExhausted)
            return kContextIdentityExhausted;
        if (current == std::numeric_limits<std::uint64_t>::max()) {
            // Latch exhaustion while yielding the last value, so a concurrent or
            // later claim observes the marker instead of a wrapped counter.
            if (!cursor.compare_exchange_strong(current, kContextIdentityExhausted,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
                continue;
            }
            return current;
        }
        if (cursor.compare_exchange_weak(current, current + 1, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            return current;
        }
    }
}

ContextIdentity allocate_context_identity() noexcept {
    const std::uint64_t value = claim_context_identity(g_context_identity_cursor);
    if (value == kContextIdentityExhausted) {
        context_identity_exhausted_fail_fast();
    }
    return ContextIdentity{value};
}

}
