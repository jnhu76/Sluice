#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace sluice::async::detail::research595 {

enum class Counter : std::size_t {
    dispatch_passes,
    poll_passes,
    empty_polls,
    sqe_preparations,
    sqes_deferred,
    flushes,
    submit_attempts,
    actual_enters,
    wait_attempts,
    cqe_reap_attempts,
    cqes_reaped,
    progress_handoffs,
    wakeups,
    wakeup_writes,
    wakeup_coalesced,
    count,
};

inline constexpr std::size_t kCounterCount = static_cast<std::size_t>(Counter::count);

inline constexpr const char* kCounterNames[kCounterCount] = {
    "dispatch_passes",  "poll_passes",     "empty_polls",    "sqe_preparations",
    "sqes_deferred",    "flushes",         "submit_attempts", "actual_enters",
    "wait_attempts",    "cqe_reap_attempts", "cqes_reaped",   "progress_handoffs",
    "wakeups",          "wakeup_writes",     "wakeup_coalesced",
};

inline std::atomic<std::uint64_t> g_counts[kCounterCount];
inline std::atomic<bool> g_enabled{false};

inline void enable_from_env() noexcept {
    const char* value = std::getenv("SLUICE_595_COUNTERS");
    g_enabled.store(value != nullptr && value[0] == '1', std::memory_order_relaxed);
}

inline bool enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

inline void disable() noexcept { g_enabled.store(false, std::memory_order_relaxed); }

inline void reset() noexcept {
    for (auto& counter : g_counts) {
        counter.store(0, std::memory_order_relaxed);
    }
}

inline void bump(Counter counter) noexcept {
    if (__builtin_expect(g_enabled.load(std::memory_order_relaxed), 0)) {
        g_counts[static_cast<std::size_t>(counter)].fetch_add(1, std::memory_order_relaxed);
    }
}

inline void dump(const char* tag, std::size_t rep) noexcept {
    if (!enabled()) {
        return;
    }
    std::fprintf(stderr, "SLUICE595_COUNTERS tag=%s rep=%zu", tag, rep);
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        std::fprintf(stderr, " %s=%llu", kCounterNames[i],
                     static_cast<unsigned long long>(
                         g_counts[i].load(std::memory_order_relaxed)));
    }
    std::fprintf(stderr, "\n");
}

} // namespace sluice::async::detail::research595

#define SLUICE595_BUMP(counter)                                                        \
    ::sluice::async::detail::research595::bump(                                        \
        ::sluice::async::detail::research595::Counter::counter)
