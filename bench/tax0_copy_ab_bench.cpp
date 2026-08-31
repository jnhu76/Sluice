// TAX-0 COPY-AB-1 (#250 T0-U-ROUTER follow-up): application-level copy A/B.
// Research instrument — drives the REAL sluice-copy engine
// (run_pipelined_copy_with_backend, the SAME copy_task the CLI uses) with an
// injected backend as the single selectable variable:
//
//   --backend uring-r0    REAL UringAsyncBackend + production_baseline router
//   --backend uring-r1    REAL UringAsyncBackend + reverse_scan router (#256
//                         research seam; the production router is untouched)
//   --backend threadpool  ThreadPoolBackend() default construction — the exact
//                         production CLI backend (capacity-effect control)
//
// The measured region is the whole engine call (Runtime build/start/submit/
// wait/drain/join + the copy). Source generation (--generate) and any content
// verification live OUTSIDE this process in official runs (the runner wraps
// only the copy process in perf stat; `cmp` runs after it, unmeasured).
//
// Same-work witness (fail-closed): bytes_copied == expected size;
// read_ops in [chunks, chunks + P] (beyond-EOF zero reads are legal
// copy-algorithm work in the EOF window, bounded by the P slots);
// write_ops == chunks; short_writes == 0 (regular-file copies on the
// campaign filesystems; a violation fails the process).
//
// Fail-closed real-ring rule: uring modes require SLUICE_HAS_LIBURING and
// available() == true, checked before the backend is handed over; there is no
// stub fallback. threadpool mode records real_uring=false by construction.
//
// Links sluice_async_internal_testing (never the production sluice_async):
// the router mode seam exists only under SLUICE_ASYNC_INTERNAL_TESTING and
// the whole binary is research-only. It is NOT part of the installed /
// public sluice-copy surface.

#include "copy_task.hpp"

#include <sluice/async/uring_backend.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

[[noreturn]] void bench_fatal(const char* what, int err) {
    std::fprintf(stderr, "tax0_copy_ab_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(3);
}

[[noreturn]] void bench_semantic(const char* what) {
    std::fprintf(stderr, "tax0_copy_ab_bench: semantic failure: %s\n", what);
    std::exit(3);
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Deterministic workload bytes — IDENTICAL generator to the TAX-0 benches
// (tax0_capacity_bench / tax0u0_router_bench / shootout: same seed, same
// 4 KiB splitmix64 master block), so campaign bytes are reproducible and
// consistent across the research line.
constexpr std::size_t kBlock = 4096;
constexpr std::uint64_t kSeed = 0xE1E1E1E121212121ull;

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

enum class Backend { uring_r0, uring_r1, threadpool };

struct Config {
    Backend backend = Backend::uring_r0;
    std::size_t buffer_size = 4096;
    std::size_t pipeline_depth = 8;
    std::size_t request_capacity = 0;  // uring only
    unsigned queue_depth = 64;         // uring only
    unsigned workers = 1;
    std::size_t expected_bytes = 0;
    std::size_t reps = 1;
    std::string src;
    std::string dst;
    bool generate = false;
};

std::size_t parse_size(const char* s) {
    return static_cast<std::size_t>(std::strtoull(s, nullptr, 0));
}

void fill_master_block(std::byte* block) {
    auto* words = reinterpret_cast<std::uint64_t*>(block);
    for (std::size_t i = 0; i < kBlock / sizeof(std::uint64_t); ++i)
        words[i] = splitmix64(kSeed + i);
}

// Unmeasured helper: materialize the deterministic source file. Never run
// inside a perf-measured region.
void generate_source(const Config& cfg) {
    std::vector<std::byte> master(kBlock);
    fill_master_block(master.data());
    int fd = ::open(cfg.src.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) bench_fatal("open source for generation", errno);
    std::vector<std::byte> chunk(std::min<std::size_t>(cfg.expected_bytes, 4u << 20));
    for (std::size_t off = 0; off < chunk.size(); off += kBlock)
        std::memcpy(chunk.data() + off, master.data(), kBlock);
    std::size_t left = cfg.expected_bytes;
    while (left > 0) {
        std::size_t n = std::min(left, chunk.size());
        ssize_t w = ::write(fd, chunk.data(), n);
        if (w < 0 || static_cast<std::size_t>(w) != n)
            bench_fatal("write generated source", errno);
        left -= n;
    }
    if (::close(fd) != 0) bench_fatal("close generated source", errno);
}

struct RepResult {
    std::uint64_t wall_ns = 0;
    std::uint64_t bytes_copied = 0;
    std::uint64_t read_ops = 0;
    std::uint64_t write_ops = 0;
    std::uint64_t short_writes = 0;
};

RepResult run_one_rep(const Config& cfg, int src_fd, int dst_fd) {
    const auto t0 = now_ns();
    std::unique_ptr<sluice::async::AsyncBackend> backend;
    if (cfg.backend == Backend::threadpool) {
        backend = std::make_unique<sluice::async::ThreadPoolBackend>();
    } else {
        auto ub = std::make_unique<sluice::async::UringAsyncBackend>(
            sluice::async::UringConfig{cfg.request_capacity, cfg.queue_depth});
        if (!ub->available()) {
            std::fprintf(stderr,
                         "tax0_copy_ab_bench: uring backend did not initialize "
                         "a real ring (available()==false) — fail closed\n");
            std::exit(3);
        }
        // Install the candidate BEFORE any runtime drives the backend
        // (quiescent fresh backend, single-threaded moment).
        ub->set_router_fix_mode_for_test(
            cfg.backend == Backend::uring_r1
                ? sluice::async::UringAsyncBackend::RouterFixModeForTest::
                    reverse_scan
                : sluice::async::UringAsyncBackend::RouterFixModeForTest::
                    production_baseline);
        backend = std::move(ub);
    }
    auto r = sluice_copy::run_pipelined_copy_with_backend(
        src_fd, dst_fd, cfg.buffer_size, cfg.pipeline_depth, cfg.workers,
        sluice_copy::SyncPolicy::none, std::move(backend));
    const auto wall = now_ns() - t0;
    if (!r.has_value()) {
        std::fprintf(stderr, "tax0_copy_ab_bench: copy failed: %s (os_errno=%d)\n",
                     std::string(sluice::to_string(r.error().code)).c_str(),
                     r.error().os_errno);
        std::exit(3);
    }
    RepResult out;
    out.wall_ns = wall;
    out.bytes_copied = r.value().bytes_copied;
    out.read_ops = r.value().read_ops;
    out.write_ops = r.value().write_ops;
    out.short_writes = r.value().short_writes;
    return out;
}

const char* backend_name(Backend b) {
    switch (b) {
    case Backend::uring_r0: return "uring-r0";
    case Backend::uring_r1: return "uring-r1";
    case Backend::threadpool: return "threadpool";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) bench_semantic("missing value for argument");
            return argv[++i];
        };
        if (a == "--backend") {
            const std::string v = next();
            if (v == "uring-r0") cfg.backend = Backend::uring_r0;
            else if (v == "uring-r1") cfg.backend = Backend::uring_r1;
            else if (v == "threadpool") cfg.backend = Backend::threadpool;
            else bench_semantic("unknown --backend");
        } else if (a == "--buffer-size") {
            cfg.buffer_size = parse_size(next());
        } else if (a == "--pipeline-depth") {
            cfg.pipeline_depth = parse_size(next());
        } else if (a == "--request-capacity") {
            cfg.request_capacity = parse_size(next());
        } else if (a == "--queue-depth") {
            cfg.queue_depth = static_cast<unsigned>(parse_size(next()));
        } else if (a == "--workers") {
            cfg.workers = static_cast<unsigned>(parse_size(next()));
        } else if (a == "--expected-bytes") {
            cfg.expected_bytes = parse_size(next());
        } else if (a == "--reps") {
            cfg.reps = parse_size(next());
        } else if (a == "--src") {
            cfg.src = next();
        } else if (a == "--dst") {
            cfg.dst = next();
        } else if (a == "--generate") {
            cfg.generate = true;
        } else {
            bench_semantic("unknown argument");
        }
    }
    if (cfg.src.empty()) bench_semantic("--src is required");
    if (cfg.expected_bytes == 0) bench_semantic("--expected-bytes is required");

    if (cfg.generate) {
        generate_source(cfg);
        return 0;
    }

    if (cfg.buffer_size == 0 || cfg.pipeline_depth == 0 || cfg.workers == 0)
        bench_semantic("buffer/depth/workers must be > 0");
    if (cfg.backend != Backend::threadpool) {
        if (cfg.request_capacity == 0 || cfg.queue_depth == 0)
            bench_semantic("uring modes need --request-capacity/--queue-depth");
    }
    if (cfg.reps == 0) bench_semantic("--reps must be >= 1");

    const std::size_t chunks =
        (cfg.expected_bytes + cfg.buffer_size - 1) / cfg.buffer_size;

    int src_fd = ::open(cfg.src.c_str(), O_RDONLY);
    if (src_fd < 0) bench_fatal("open source", errno);
    int dst_fd = ::open(cfg.dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) bench_fatal("open destination", errno);

    std::vector<RepResult> results;
    for (std::size_t r = 0; r < cfg.reps; ++r) {
        // Each rep rewrites the destination from offset 0 (positional writes);
        // the official campaign uses exactly one rep per process.
        if (::lseek(dst_fd, 0, SEEK_SET) == static_cast<off_t>(-1))
            bench_fatal("lseek destination", errno);
        results.push_back(run_one_rep(cfg, src_fd, dst_fd));
    }

    struct rusage ru;
    if (::getrusage(RUSAGE_SELF, &ru) != 0) bench_fatal("getrusage", errno);
    const std::uint64_t user_ns =
        static_cast<std::uint64_t>(ru.ru_utime.tv_sec) * 1000000000ull +
        static_cast<std::uint64_t>(ru.ru_utime.tv_usec) * 1000ull;
    const std::uint64_t sys_ns =
        static_cast<std::uint64_t>(ru.ru_stime.tv_sec) * 1000000000ull +
        static_cast<std::uint64_t>(ru.ru_stime.tv_usec) * 1000ull;

    ::close(src_fd);
    ::close(dst_fd);

    // Same-work witness, fail-closed. bytes_copied must equal the expected
    // file size in every rep; read_ops is bounded by the copy algorithm
    // (chunk reads + at most P beyond-EOF zero reads); every data chunk is
    // written exactly once (no short writes expected on regular files).
    bool all_ok = true;
    for (const auto& r : results) {
        if (r.bytes_copied != cfg.expected_bytes) all_ok = false;
        if (r.read_ops < chunks || r.read_ops > chunks + cfg.pipeline_depth)
            all_ok = false;
        if (r.write_ops != chunks) all_ok = false;
        if (r.short_writes != 0) all_ok = false;
    }
    if (!all_ok) bench_semantic("same-work witness violation");

    auto esc = [](const std::string& s) {
        std::string o;
        for (char ch : s) {
            switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            default: o += ch;
            }
        }
        return o;
    };

    std::string out;
    out += "{\n";
    out += "  \"bench\": \"tax0_copy_ab_bench\",\n";
    out += "  \"bench_version\": 1,\n";
    out += "  \"experiment\": \"TAX-0-COPY-AB-1\",\n";
    out += std::string("  \"backend\": \"") + backend_name(cfg.backend) + "\",\n";
    out += std::string("  \"real_uring\": ") +
           (cfg.backend == Backend::threadpool ? "false" : "true") + ",\n";
    out += "  \"buffer_size\": " + std::to_string(cfg.buffer_size) + ",\n";
    out += "  \"pipeline_depth\": " + std::to_string(cfg.pipeline_depth) + ",\n";
    out += "  \"request_capacity\": " +
           std::to_string(cfg.request_capacity) + ",\n";
    out += "  \"queue_depth\": " + std::to_string(cfg.queue_depth) + ",\n";
    out += "  \"workers\": " + std::to_string(cfg.workers) + ",\n";
    out += "  \"expected_bytes\": " + std::to_string(cfg.expected_bytes) + ",\n";
    out += "  \"chunks\": " + std::to_string(chunks) + ",\n";
    out += "  \"reps\": " + std::to_string(cfg.reps) + ",\n";
    out += "  \"src\": \"" + esc(cfg.src) + "\",\n";
    out += "  \"dst\": \"" + esc(cfg.dst) + "\",\n";
    // Process-level OS accounting (getrusage, whole process — the same
    // convention as the TAX-0 benches; perf counters remain authoritative).
    out += "  \"user_ns\": " + std::to_string(user_ns) + ",\n";
    out += "  \"sys_ns\": " + std::to_string(sys_ns) + ",\n";
    out += "  \"maxrss_kb\": " + std::to_string(ru.ru_maxrss) + ",\n";
    out += "  \"same_work\": true,\n";
    out += "  \"reps_out\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "    {\"wall_ns\": %llu, \"bytes_copied\": %llu, "
                      "\"read_ops\": %llu, \"write_ops\": %llu, "
                      "\"short_writes\": %llu}%s\n",
                      static_cast<unsigned long long>(r.wall_ns),
                      static_cast<unsigned long long>(r.bytes_copied),
                      static_cast<unsigned long long>(r.read_ops),
                      static_cast<unsigned long long>(r.write_ops),
                      static_cast<unsigned long long>(r.short_writes),
                      i + 1 < results.size() ? "," : "");
        out += buf;
    }
    out += "  ]\n}\n";
    std::fputs(out.c_str(), stdout);
    return 0;
}
