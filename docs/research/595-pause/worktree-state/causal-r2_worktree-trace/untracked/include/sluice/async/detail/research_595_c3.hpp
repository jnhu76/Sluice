#pragma once

// Research-only #595 causal-r2 C3 aggregation probes (kernel-path syscall
// face: eventfd drain reads, park poll(2), notification writes). Not product
// code. Compiled only under SLUICE_595_C3_COUNTERS; runtime-enabled with
// SLUICE_595_C3=1 so the same binary yields ON and OFF blocks.

#if defined(SLUICE_595_C3_COUNTERS)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <x86intrin.h>

namespace sluice::async::detail::research595::c3 {

enum class Agg : std::size_t {
    read_ok,
    read_eagain,
    park_poll,
    write_fd,
    count,
};

inline constexpr std::size_t kAggCount = static_cast<std::size_t>(Agg::count);

inline constexpr const char* kAggNames[kAggCount] = {
    "read_ok",
    "read_eagain",
    "park_poll",
    "write_fd",
};

// Same thresholds as the C1 probes: 3000 ticks ~= 1.04 us at 2.893 GHz; a
// park_poll interval above it is sleep-dominated, so its clean subset is the
// immediately-returning poll population only.
inline constexpr std::uint64_t kCleanTicks = 3000;
inline constexpr std::uint64_t kHugeTicks = 30000;

struct Entry {
    std::uint64_t count = 0;
    std::uint64_t tsc_sum = 0;
    std::uint64_t tsc_max = 0;
    std::uint64_t clean_count = 0;
    std::uint64_t clean_sum = 0;
    std::uint64_t huge_count = 0;
};

// All measured-path calls run on the bench driver thread (progress owner),
// matching the C1 determination; plain thread-local counters avoid RMW bias.
inline thread_local Entry g_aggs[kAggCount];

inline thread_local std::uint64_t g_drain_park = 0;
inline thread_local std::uint64_t g_drain_ack = 0;
inline thread_local std::uint64_t g_poll_wake_pollin = 0;
inline thread_local std::uint64_t g_poll_timeout = 0;
inline thread_local std::uint64_t g_poll_eintr = 0;
inline thread_local std::uint64_t g_poll_other = 0;
inline thread_local std::uint64_t g_read_value_sum = 0;

inline bool g_enabled = false;
inline std::uint64_t g_tsc_hz = 0;
inline std::uint64_t g_empty_pair_tsc = 0;

inline std::uint64_t tsc_begin() noexcept {
    _mm_lfence();
    return static_cast<std::uint64_t>(__rdtsc());
}

inline std::uint64_t tsc_end() noexcept {
    unsigned aux = 0;
    const std::uint64_t ticks = static_cast<std::uint64_t>(__rdtscp(&aux));
    _mm_lfence();
    return ticks;
}

inline void record(Agg agg, std::uint64_t t0) noexcept {
    const std::uint64_t delta = tsc_end() - t0;
    Entry& entry = g_aggs[static_cast<std::size_t>(agg)];
    entry.count += 1;
    entry.tsc_sum += delta;
    if (delta > entry.tsc_max) {
        entry.tsc_max = delta;
    }
    if (delta <= kCleanTicks) {
        entry.clean_count += 1;
        entry.clean_sum += delta;
    } else if (delta > kHugeTicks) {
        entry.huge_count += 1;
    }
}

inline void count(std::uint64_t& counter) noexcept {
    if (g_enabled) {
        counter += 1;
    }
}

inline std::uint64_t monotonic_ns() noexcept {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

inline void calibrate() noexcept {
    if (g_tsc_hz != 0) {
        return;
    }
    const std::uint64_t ns0 = monotonic_ns();
    const std::uint64_t t0 = tsc_begin();
    for (;;) {
        if (monotonic_ns() - ns0 >= 5000000ull) {
            break;
        }
    }
    const std::uint64_t t1 = tsc_end();
    const std::uint64_t ns1 = monotonic_ns();
    const std::uint64_t elapsed_ns = ns1 - ns0;
    if (elapsed_ns > 0) {
        g_tsc_hz = static_cast<std::uint64_t>(
            (static_cast<double>(t1 - t0) * 1.0e9) / static_cast<double>(elapsed_ns));
    }
    constexpr std::uint64_t kPairs = 20000;
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < kPairs; ++i) {
        const std::uint64_t a = tsc_begin();
        sum += tsc_end() - a;
    }
    g_empty_pair_tsc = sum / kPairs;
}

inline void init_from_env() noexcept {
    const char* value = std::getenv("SLUICE_595_C3");
    g_enabled = value != nullptr && value[0] == '1';
    if (g_enabled) {
        calibrate();
    }
}

inline void enable_from_env() noexcept {
    const char* value = std::getenv("SLUICE_595_C3");
    g_enabled = value != nullptr && value[0] == '1';
}

inline void disable() noexcept { g_enabled = false; }

inline void reset() noexcept {
    for (auto& entry : g_aggs) {
        entry = Entry{};
    }
    g_drain_park = 0;
    g_drain_ack = 0;
    g_poll_wake_pollin = 0;
    g_poll_timeout = 0;
    g_poll_eintr = 0;
    g_poll_other = 0;
    g_read_value_sum = 0;
}

inline void dump(const char* tag, std::size_t rep) noexcept {
    if (!g_enabled) {
        return;
    }
    std::fprintf(stderr, "SLUICE595_C3 tag=%s rep=%zu tsc_hz=%llu empty_pair_tsc=%llu",
                 tag, rep,
                 static_cast<unsigned long long>(g_tsc_hz),
                 static_cast<unsigned long long>(g_empty_pair_tsc));
    for (std::size_t i = 0; i < kAggCount; ++i) {
        const Entry& entry = g_aggs[i];
        std::fprintf(stderr,
                     " %s_count=%llu %s_tsc_sum=%llu %s_tsc_max=%llu "
                     "%s_clean_count=%llu %s_clean_tsc_sum=%llu %s_huge_count=%llu",
                     kAggNames[i],
                     static_cast<unsigned long long>(entry.count),
                     kAggNames[i],
                     static_cast<unsigned long long>(entry.tsc_sum),
                     kAggNames[i],
                     static_cast<unsigned long long>(entry.tsc_max),
                     kAggNames[i],
                     static_cast<unsigned long long>(entry.clean_count),
                     kAggNames[i],
                     static_cast<unsigned long long>(entry.clean_sum),
                     kAggNames[i],
                     static_cast<unsigned long long>(entry.huge_count));
    }
    std::fprintf(stderr,
                 " drain_park=%llu drain_ack=%llu poll_wake_pollin=%llu "
                 "poll_timeout=%llu poll_eintr=%llu poll_other=%llu "
                 "read_value_sum=%llu\n",
                 static_cast<unsigned long long>(g_drain_park),
                 static_cast<unsigned long long>(g_drain_ack),
                 static_cast<unsigned long long>(g_poll_wake_pollin),
                 static_cast<unsigned long long>(g_poll_timeout),
                 static_cast<unsigned long long>(g_poll_eintr),
                 static_cast<unsigned long long>(g_poll_other),
                 static_cast<unsigned long long>(g_read_value_sum));
}

} // namespace sluice::async::detail::research595::c3

#endif // SLUICE_595_C3_COUNTERS

#if defined(SLUICE_595_C3_COUNTERS)
#define SLUICE595_C3_COUNT(counter)                                                    \
    (::sluice::async::detail::research595::c3::count(                                   \
        ::sluice::async::detail::research595::c3::g_##counter))
#define SLUICE595_C3_ENABLED() (::sluice::async::detail::research595::c3::g_enabled)
#define SLUICE595_C3_TSC_BEGIN() (::sluice::async::detail::research595::c3::tsc_begin())
#define SLUICE595_C3_TSC_RECORD(agg, t0)                                               \
    (::sluice::async::detail::research595::c3::record(                                 \
        ::sluice::async::detail::research595::c3::Agg::agg, (t0)))
#else
#define SLUICE595_C3_COUNT(counter) ((void)0)
#define SLUICE595_C3_ENABLED() (false)
#define SLUICE595_C3_TSC_BEGIN() (0ull)
#define SLUICE595_C3_TSC_RECORD(agg, t0) ((void)0)
#endif
