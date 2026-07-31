// sluice-copy Version B deterministic pipeline stress test (Phase 8).
//
// Drives run_pipelined_copy over a randomized-but-deterministic matrix of
// sizes / buffers / depths / workers / sync policies and verifies byte-for-byte
// content equality plus CopyStats sanity on REAL temporary files with the
// production ThreadPoolBackend.
//
// Determinism: every parameter sequence derives from a single seed via a fixed
// xorshift64 stream, so `--seed N --iterations M` reproduces the exact same
// workload (and therefore any failure) on any machine. The smoke case runs a
// fixed table independent of the flags so the binary never runs vacuously.
//
// Usage:
//   sluice_copy_pipeline_stress_test [--seed <u64>] [--iterations <u64>]
//   --seed <u64>       PRNG seed (default 0x5A17CE; 0 means default)
//   --iterations <u64> randomized copy rounds (default 10)
//   --help             show usage
//
// Defaults are tuned so the binary stays fast enough for the default test
// group and for sanitizer gates; the nightly hardening run passes larger
// --iterations values. Run under TSan to catch races.
#include "harness.hpp"

#include "copy_task.hpp"  // apps/sluice-copy public header

#include <sluice/error.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace sluice_copy;

namespace {

struct StressArgs {
    std::uint64_t seed = 0x5A17CEu;
    std::uint64_t iterations = 10;
};
StressArgs g_args;

// Fixed xorshift64* stream. Deterministic per seed; a nonzero seed is enforced
// at the start of the case (xorshift degenerates on all-zero state).
std::uint64_t rng_state = 0x5A17CEu;
std::uint64_t rng_next() {
    std::uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}
std::size_t pick(std::size_t lo, std::size_t hi) {
    return lo + static_cast<std::size_t>(rng_next() % (hi - lo + 1));
}

// RAII anonymous temp file.
struct TempFile {
    int fd;
    TempFile() {
        char p[] = "/tmp/sluice_pst_XXXXXX";
        fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::unlink(p);
    }
    ~TempFile() { if (fd >= 0) ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// Deterministic byte pattern mixed with the seed (failures reproducible).
std::byte byte_at(std::uint64_t seed, std::size_t i) {
    unsigned char b = static_cast<unsigned char>(
        (i * 31 + 7 + static_cast<std::size_t>(seed)) & 0xFF);
    return (i % 7 == 0) ? std::byte{0} : std::byte{b};
}

void seed_file(int fd, std::size_t n, std::uint64_t seed) {
    std::vector<std::byte> data(n);
    for (std::size_t i = 0; i < n; ++i) data[i] = byte_at(seed, i);
    if (n > 0) {
        ssize_t w = ::pwrite(fd, data.data(), n, 0);
        SLUICE_CHECK(static_cast<std::size_t>(w) == n);
    }
}

bool files_equal(int a, int b, std::size_t n) {
    if (n == 0) return true;
    std::vector<std::byte> da(n), db(n);
    ssize_t ra = ::pread(a, da.data(), n, 0);
    ssize_t rb = ::pread(b, db.data(), n, 0);
    if (ra != static_cast<ssize_t>(n) || rb != static_cast<ssize_t>(n))
        return false;
    return std::memcmp(da.data(), db.data(), n) == 0;
}

struct RoundParams {
    std::uint64_t round;
    std::size_t buf;
    std::size_t depth;
    std::size_t n;
    unsigned workers;
    SyncPolicy sync;
};

// Record a failure that carries the exact round parameters so `--seed`
// reproduces it.
void record_round_failure(const RoundParams& p, const char* what) {
    char msg[320];
    std::snprintf(msg, sizeof(msg),
                  "round %llu %s (buf=%llu depth=%llu workers=%u n=%llu sync=%d)",
                  static_cast<unsigned long long>(p.round), what,
                  static_cast<unsigned long long>(p.buf),
                  static_cast<unsigned long long>(p.depth), p.workers,
                  static_cast<unsigned long long>(p.n), static_cast<int>(p.sync));
    ::sluice_test::record_failure(__FILE__, __LINE__, "pipeline_stress", msg);
}

// One randomized round: pick params from the stream, copy, verify.
void run_one_round(std::uint64_t seed, std::uint64_t round) {
    static constexpr std::size_t kBufs[] = {1u, 7u, 64u, 512u, 4096u};
    static constexpr std::size_t kDepths[] = {1u, 2u, 3u, 4u, 8u};
    static constexpr unsigned kWorkers[] = {1u, 2u};
    static constexpr SyncPolicy kSyncs[] = {SyncPolicy::none, SyncPolicy::data,
                                            SyncPolicy::all};

    RoundParams p;
    p.round = round;
    p.buf = kBufs[pick(0, 4)];
    p.depth = kDepths[pick(0, 4)];
    p.workers = kWorkers[pick(0, 1)];
    p.sync = kSyncs[pick(0, 2)];

    // Cap the byte count so tiny buffers cannot explode the op count (a 1-byte
    // buffer costs ~2 thread-spawned ops per byte on ThreadPoolBackend).
    if (p.buf <= 7) {
        p.n = pick(0, 8192);
    } else if (p.buf <= 64) {
        p.n = pick(0, 32768);
    } else {
        p.n = pick(0, 131072);
    }
    // A quarter of the rounds use boundary sizes that exercise edge cases.
    switch (rng_next() % 4) {
        case 0: p.n = 0; break;
        case 1: p.n = 1; break;
        case 2: p.n = p.buf - 1; break;
        case 3: p.n = p.depth * p.buf; break;
        default: break;
    }

    TempFile src, dst;
    seed_file(src.fd, p.n, seed);
    auto r = run_pipelined_copy(src.fd, dst.fd, p.buf, p.depth, p.workers, p.sync);
    if (!r.has_value()) {
        record_round_failure(p, "copy returned an error");
        return;
    }
    const auto& stats = r.value();
    if (stats.bytes_copied != p.n) {
        record_round_failure(p, "bytes_copied mismatch");
        return;
    }
    if (p.n > 0 && (stats.read_ops == 0 || stats.write_ops == 0)) {
        record_round_failure(p, "no I/O ops recorded for non-empty copy");
        return;
    }
    if (stats.short_writes != 0) {
        record_round_failure(p, "unexpected short write on regular file");
        return;
    }
    if (!files_equal(src.fd, dst.fd, p.n)) {
        record_round_failure(p, "source/destination differ");
        return;
    }
}

}  // namespace

// The main deterministic matrix. Seeded by --seed; every round derives its
// parameters from the fixed stream, so a failure at round k reproduces.
SLUICE_TEST_CASE(pipeline_stress_deterministic_copy) {
    rng_state = g_args.seed ? g_args.seed : 0x5A17CEu;
    const std::uint64_t seed = rng_state;
    for (std::uint64_t i = 0; i < g_args.iterations; ++i) {
        run_one_round(seed, i);
    }
}

// Fixed table, independent of --seed/--iterations, so the binary always does
// meaningful work even when invoked with --iterations 0.
SLUICE_TEST_CASE(pipeline_stress_smoke) {
    constexpr std::size_t kB = 4096;
    for (std::size_t depth : {1u, 2u, 8u}) {
        for (std::size_t n : {std::size_t{0}, std::size_t{1}, kB - 1, kB,
                              depth * kB + 1}) {
            TempFile src, dst;
            seed_file(src.fd, n, 0xC0FFEEu);
            auto r = run_pipelined_copy(src.fd, dst.fd, kB, depth, 1,
                                        SyncPolicy::none);
            SLUICE_CHECK(r.has_value());
            SLUICE_CHECK(r.value().bytes_copied == n);
            SLUICE_CHECK(files_equal(src.fd, dst.fd, n));
        }
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) {
            g_args.seed = std::strtoull(argv[++i], nullptr, 0);
        } else if (a == "--iterations" && i + 1 < argc) {
            g_args.iterations = std::strtoull(argv[++i], nullptr, 0);
        } else if (a == "--help") {
            std::printf("usage: %s [--seed <u64>] [--iterations <u64>]\n",
                        argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }
    std::printf("pipeline stress: seed=%llu iterations=%llu\n",
                static_cast<unsigned long long>(g_args.seed),
                static_cast<unsigned long long>(g_args.iterations));
    return ::sluice_test::run_all();
}
