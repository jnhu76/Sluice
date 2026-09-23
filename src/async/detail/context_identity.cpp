#include <sluice/async/detail/context_identity.hpp>

#include <sluice/async/detail/fail_fast.hpp>

#include <atomic>
#include <cstdint>

namespace sluice::async::detail {

namespace {

// 0 is the unadopted slot-table value, so the domain hands out 1..MAX exactly
// once and terminates instead of wrapping onto an identity that is still live.
std::atomic<std::uint64_t> g_next_context_identity{1};

}

ContextIdentity allocate_context_identity() noexcept {
    const std::uint64_t value = g_next_context_identity.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) {
        context_identity_exhausted_fail_fast();
    }
    return ContextIdentity{value};
}

}
