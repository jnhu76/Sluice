#pragma once

// Research-only #595 causal-r2 cross-layer correlation event ring. Records
// per-request lifecycle events (accept, SQE prepare, defer/immediate, flush,
// CQE reap, publish, application take) into one bounded per-process ring so
// they can be joined offline against kernel io_uring tracepoints keyed by
// (ring_fd/user_data). Not product code. Compiled only under
// SLUICE_595_TRACE_BUILD; runtime-enabled with SLUICE_595_TRACE=1 so the same
// binary yields traced and untraced runs.

#if defined(SLUICE_595_TRACE_BUILD)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

namespace sluice::async::detail::research595::trace {

enum class Evt : std::uint8_t {
    accept = 1,
    sqe_prep = 2,
    defer = 3,
    immediate = 4,
    flush = 5,
    reap = 6,
    published = 7,
    take = 8,
};

inline constexpr const char* kEvtNames[] = {
    "?", "accept", "sqe_prep", "defer", "immediate", "flush", "reap", "published", "take",
};

inline constexpr std::uint64_t kCap = std::uint64_t{1} << 20;

struct Record {
    std::uint64_t seq;
    std::uint64_t ts_ns;
    std::uint64_t cookie;
    std::uint64_t gen;
    std::uint64_t aux;
    std::uint32_t tid;
    std::int32_t ring_fd;
    std::uint32_t slot;
    std::uint32_t phase;
    std::uint8_t evt;
    std::uint8_t cpu;
    std::uint16_t flags;
};

inline constexpr std::uint16_t kFlagControlCookie = 1u << 0;

inline Record g_ring[kCap];
inline std::atomic<std::uint64_t> g_head{0};
inline std::atomic<bool> g_enabled{false};
inline std::atomic<std::uint32_t> g_phase{0};
inline std::uint64_t g_tsc_hz_probe = 0;

inline std::uint64_t mono_ns() noexcept {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

inline void write(Evt evt, std::uint32_t slot, std::uint64_t gen, std::uint64_t cookie,
                  std::uint64_t aux, int ring_fd, std::uint16_t flags) noexcept {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const std::uint64_t seq = g_head.fetch_add(1, std::memory_order_relaxed);
    Record& r = g_ring[seq & (kCap - 1)];
    r.seq = seq;
    r.ts_ns = mono_ns();
    r.cookie = cookie;
    r.gen = gen;
    r.aux = aux;
    r.tid = static_cast<std::uint32_t>(::gettid());
    r.ring_fd = ring_fd;
    r.slot = slot;
    r.phase = g_phase.load(std::memory_order_relaxed);
    r.evt = static_cast<std::uint8_t>(evt);
    r.cpu = static_cast<std::uint8_t>(::sched_getcpu());
    r.flags = flags;
}

inline void next_phase() noexcept {
    g_phase.fetch_add(1, std::memory_order_relaxed);
}

inline void init_from_env() noexcept {
    const char* value = std::getenv("SLUICE_595_TRACE");
    g_enabled.store(value != nullptr && value[0] == '1', std::memory_order_relaxed);
}

inline void disable() noexcept {
    g_enabled.store(false, std::memory_order_relaxed);
}

inline void dump(const char* path) noexcept {
    const std::uint64_t head = g_head.load(std::memory_order_relaxed);
    const std::uint64_t begin = head > kCap ? head - kCap : 0;
    const std::uint64_t dropped = begin;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "S595_TRACE_DUMP error=open_failed path=%s errno=%d\n", path, errno);
        return;
    }
    std::uint64_t written = 0;
    constexpr std::size_t kChunkRecords = 8192;
    static Record chunk[kChunkRecords];
    for (std::uint64_t at = begin; at < head;) {
        const std::uint64_t take = head - at < kChunkRecords ? head - at : kChunkRecords;
        for (std::uint64_t i = 0; i < take; ++i) {
            chunk[i] = g_ring[(at + i) & (kCap - 1)];
        }
        const std::size_t bytes = static_cast<std::size_t>(take) * sizeof(Record);
        if (::write(fd, chunk, bytes) < 0) {
            std::fprintf(stderr, "S595_TRACE_DUMP error=write_failed errno=%d\n", errno);
            ::close(fd);
            return;
        }
        written += take;
        at += take;
    }
    ::close(fd);
    std::fprintf(stderr,
                 "S595_TRACE_DUMP path=%s sizeof_record=%zu head=%llu kept=%llu dropped=%llu\n",
                 path, sizeof(Record), static_cast<unsigned long long>(head),
                 static_cast<unsigned long long>(written),
                 static_cast<unsigned long long>(dropped));
}

}  // namespace sluice::async::detail::research595::trace

#define S595_TR(evt, slot, gen, cookie, aux, ring_fd, flags)                             \
    (::sluice::async::detail::research595::trace::write(                                  \
        (evt), (slot), (gen), (cookie), (aux), (ring_fd), (flags)))

#else

#define S595_TR(evt, slot, gen, cookie, aux, ring_fd, flags) ((void)0)

namespace sluice::async::detail::research595::trace {
inline void next_phase() noexcept {}
inline void init_from_env() noexcept {}
inline void disable() noexcept {}
inline void dump(const char*) noexcept {}
}  // namespace sluice::async::detail::research595::trace

#endif  // SLUICE_595_TRACE_BUILD
