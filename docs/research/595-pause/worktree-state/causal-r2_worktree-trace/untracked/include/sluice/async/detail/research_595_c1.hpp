#pragma once

// Research-only #595 causal-r2 C1 aggregation probes. Not product code.
// Compiled only under SLUICE_595_C1_COUNTERS; runtime-enabled with
// SLUICE_595_C1=1 so the same binary yields ON and OFF blocks.

#if defined(SLUICE_595_C1_COUNTERS)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <x86intrin.h>

namespace sluice::async::detail::research595::c1 {

enum class Agg : std::size_t {
    defer,
    signal,
    write,
    poll,
    submit,
    flush,
    count,
};

inline constexpr std::size_t kAggCount = static_cast<std::size_t>(Agg::count);

inline constexpr const char* kAggNames[kAggCount] = {
    "defer",
    "signal",
    "write",
    "poll",
    "submit",
    "flush",
};

// kCleanTicks = 3000 ~= 1.04 us at the measured 2.893 GHz TSC; deltas above
// it are dominated by preemption/off-CPU time and are tracked separately so a
// clean in-CPU mean can be reported next to the full mean.
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

// All measured-path calls (dispatch, poll, signal, write) run on the bench
// driver thread, so thread-local plain counters avoid atomic RMW bias.
inline thread_local Entry g_aggs[kAggCount];

inline thread_local std::uint64_t g_signal_entry = 0;
inline thread_local std::uint64_t g_signal_coalesced = 0;
inline thread_local std::uint64_t g_defer_owner_pass = 0;
inline thread_local std::uint64_t g_defer_witness_pass = 0;

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

class Scope {
  public:
    explicit Scope(Agg agg) noexcept : agg_(agg), on_(g_enabled) {
        if (on_) {
            t0_ = tsc_begin();
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() noexcept {
        if (on_) {
            record(agg_, t0_);
        }
    }

  private:
    Agg agg_;
    bool on_;
    std::uint64_t t0_ = 0;
};

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
    const char* value = std::getenv("SLUICE_595_C1");
    g_enabled = value != nullptr && value[0] == '1';
    if (g_enabled) {
        calibrate();
    }
}

inline void enable_from_env() noexcept {
    const char* value = std::getenv("SLUICE_595_C1");
    g_enabled = value != nullptr && value[0] == '1';
}

inline void disable() noexcept { g_enabled = false; }

inline void reset() noexcept {
    for (auto& entry : g_aggs) {
        entry = Entry{};
    }
    g_signal_entry = 0;
    g_signal_coalesced = 0;
    g_defer_owner_pass = 0;
    g_defer_witness_pass = 0;
}

inline void dump(const char* tag, std::size_t rep) noexcept {
    if (!g_enabled) {
        return;
    }
    std::fprintf(stderr, "SLUICE595_C1 tag=%s rep=%zu tsc_hz=%llu empty_pair_tsc=%llu",
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
                 " signal_entry=%llu signal_coalesced=%llu defer_owner_pass=%llu "
                 "defer_witness_pass=%llu\n",
                 static_cast<unsigned long long>(g_signal_entry),
                 static_cast<unsigned long long>(g_signal_coalesced),
                 static_cast<unsigned long long>(g_defer_owner_pass),
                 static_cast<unsigned long long>(g_defer_witness_pass));
}

} // namespace sluice::async::detail::research595::c1

#endif // SLUICE_595_C1_COUNTERS

#if defined(SLUICE_595_C1_COUNTERS)
#define SLUICE595_C1_SCOPE(name, agg)                                                  \
    ::sluice::async::detail::research595::c1::Scope name(                              \
        ::sluice::async::detail::research595::c1::Agg::agg)
#define SLUICE595_C1_COUNT(counter)                                                    \
    ::sluice::async::detail::research595::c1::count(                                   \
        ::sluice::async::detail::research595::c1::g_##counter)
#define SLUICE595_C1_ENABLED() (::sluice::async::detail::research595::c1::g_enabled)
#define SLUICE595_C1_TSC_BEGIN() (::sluice::async::detail::research595::c1::tsc_begin())
#define SLUICE595_C1_TSC_RECORD(agg, t0)                                               \
    (::sluice::async::detail::research595::c1::record(                                 \
        ::sluice::async::detail::research595::c1::Agg::agg, (t0)))
#else
#define SLUICE595_C1_SCOPE(name, agg) ((void)0)
#define SLUICE595_C1_COUNT(counter) ((void)0)
#define SLUICE595_C1_ENABLED() (false)
#define SLUICE595_C1_TSC_BEGIN() (0ull)
#define SLUICE595_C1_TSC_RECORD(agg, t0) ((void)0)
#endif
