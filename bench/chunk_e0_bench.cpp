// CHUNK-E0 Phase H0 (#270, prereg
// research/chunk-e0/CHUNK-E0-H0-PREREGISTRATION.md): chunk-size x pipeline
// depth performance surface of the CURRENT production buffered READ + WRITE
// copy engine on the Host-0 bare-metal x86-64 machine.
//
// This bench measures ONE module only — the REAL production engine
// (apps/sluice-copy/copy_task.cpp, run_pipelined_copy_with_backend with
// ThreadPoolBackend, workers = 1, the production CLI default). No replica
// modules, no alignment treatment, no registered buffers, no splice /
// copy_file_range / O_DIRECT / SIMD, no multi-worker (all frozen OUT in the
// preregistration).
//
// Per run: `--run --chunk C --depth D --file-bytes N` opens the src file and
// an O_TRUNC dst, times the FULL engine span (Runtime build/start/submit/
// wait/drain/join + the copy) in-process, and emits ONE JSON line on stdout
// with the same-work evidence. Same-work gates (fail-closed, exit 3):
// bytes_copied == file size, write_ops == ceil(bytes/chunk),
// read_ops in [ceil, ceil+depth], short_writes == 0.
//
// Resource metrics (prereg §8 secondary): peak RSS (ru_maxrss), user+sys CPU
// time (getrusage delta), minor/major page faults (ru_minflt/ru_majflt).
//
// Workload bytes: the TAX-0/ALIGN-E1-line generator (4 KiB splitmix64 master
// block, seed 0xE1E1E1E121212121), generated once by --generate. Production
// code is NOT modified (read-only engine call).
//
// The driver (research/chunk-e0/scripts/chunk_e0.py) wraps each run under
// `perf stat -e instructions:u,cycles:u,task-clock`, hashes src/dst post-exit
// and appends raw evidence into the immutable session.

#include "copy_task.hpp"

#include <sluice/async/threadpool_backend.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice_copy::CopyStats;
using sluice_copy::SyncPolicy;

[[noreturn]] void e0_fatal(const char* what, int err) {
    std::fprintf(stderr, "chunk_e0_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(2);
}

[[noreturn]] void e0_semantic(const char* what) {
    std::fprintf(stderr, "chunk_e0_bench: semantic failure: %s\n", what);
    std::exit(3);
}

constexpr std::size_t kBlock = 4096;
constexpr std::uint64_t kSeed = 0xE1E1E1E121212121ull;

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct Config {
    std::size_t chunk = 1u << 20;
    std::size_t depth = 1;
    std::uint64_t file_bytes = 1ull << 30;
    std::string src, dst;
    bool generate = false;
    bool run = false;
    std::string label;
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* w) -> std::string {
            if (i + 1 >= argc) e0_fatal(w, EINVAL);
            return argv[++i];
        };
        if (a == "--chunk") {
            c.chunk = std::strtoull(next("--chunk").c_str(), nullptr, 10);
        } else if (a == "--depth") {
            c.depth = std::strtoull(next("--depth").c_str(), nullptr, 10);
        } else if (a == "--file-bytes") {
            c.file_bytes = std::strtoull(next("--file-bytes").c_str(),
                                         nullptr, 10);
        } else if (a == "--src") {
            c.src = next("--src");
        } else if (a == "--dst") {
            c.dst = next("--dst");
        } else if (a == "--label") {
            c.label = next("--label");
        } else if (a == "--generate") {
            c.generate = true;
        } else if (a == "--run") {
            c.run = true;
        } else {
            e0_semantic("unknown arg");
        }
    }
    return c;
}

void generate_file(const Config& cfg) {
    std::vector<std::byte> master(kBlock);
    auto* w = reinterpret_cast<std::uint64_t*>(master.data());
    for (std::size_t i = 0; i < kBlock / 8; ++i) w[i] = splitmix64(kSeed + i);
    int fd = ::open(cfg.src.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) e0_fatal("open(generate)", errno);
    std::vector<std::byte> chunk(1u << 20);
    for (std::size_t off = 0; off < chunk.size(); off += kBlock)
        std::memcpy(chunk.data() + off, master.data(), kBlock);
    std::uint64_t written = 0;
    while (written < cfg.file_bytes) {
        std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(),
                                    cfg.file_bytes - written));
        ssize_t x = ::write(fd, chunk.data(), n);
        if (x < 0) {
            if (errno == EINTR) continue;
            e0_fatal("write(generate)", errno);
        }
        written += static_cast<std::uint64_t>(x);
    }
    if (::close(fd) != 0) e0_fatal("close(generate)", errno);
}

int run_one(const Config& cfg) {
    if (cfg.chunk == 0 || cfg.depth == 0)
        e0_semantic("chunk/depth must be > 0");
    if (cfg.chunk > sluice_copy::kMaxBufferSize ||
        cfg.depth > sluice_copy::kMaxPipelineDepth ||
        cfg.chunk * cfg.depth > sluice_copy::kMaxPipelineBytes)
        e0_semantic("cell exceeds the production copy resource limits");
    const std::uint64_t chunks =
        (cfg.file_bytes + cfg.chunk - 1) / cfg.chunk;  // ceil

    struct rusage ru0, ru1;
    if (::getrusage(RUSAGE_SELF, &ru0) != 0)
        e0_fatal("getrusage(before)", errno);

    int src_fd = ::open(cfg.src.c_str(), O_RDONLY);
    if (src_fd < 0) e0_fatal("open(src)", errno);
    int dst_fd = ::open(cfg.dst.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) e0_fatal("open(dst)", errno);

    std::uint64_t t0 = now_ns();
    auto res = sluice_copy::run_pipelined_copy_with_backend(
        src_fd, dst_fd, cfg.chunk, cfg.depth, /*workers=*/1,
        SyncPolicy::none,
        std::make_unique<sluice::async::ThreadPoolBackend>());
    const std::uint64_t total_ns = now_ns() - t0;
    if (!res.has_value()) e0_semantic("engine copy failed");
    const CopyStats st = res.value();

    // Same-work gates (prereg §7) — fail closed.
    if (st.bytes_copied != cfg.file_bytes)
        e0_semantic("bytes_copied != file size");
    if (st.write_ops != chunks) e0_semantic("write_ops != ceil(bytes/chunk)");
    if (st.read_ops < chunks || st.read_ops > chunks + cfg.depth)
        e0_semantic("read_ops out of [ceil, ceil+depth]");
    if (st.short_writes != 0) e0_semantic("short_writes != 0");

    if (::close(src_fd) != 0) e0_fatal("close(src)", errno);
    if (::close(dst_fd) != 0) e0_fatal("close(dst)", errno);

    if (::getrusage(RUSAGE_SELF, &ru1) != 0)
        e0_fatal("getrusage(after)", errno);
    const std::uint64_t utime_us =
        static_cast<std::uint64_t>(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) *
            1000000ull +
        static_cast<std::uint64_t>(ru1.ru_utime.tv_usec -
                                   ru0.ru_utime.tv_usec);
    const std::uint64_t stime_us =
        static_cast<std::uint64_t>(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) *
            1000000ull +
        static_cast<std::uint64_t>(ru1.ru_stime.tv_usec -
                                   ru0.ru_stime.tv_usec);
    const long maxrss_kb = ru1.ru_maxrss;  // KiB (Linux)
    const long minflt = ru1.ru_minflt;
    const long majflt = ru1.ru_majflt;

    // ---- one JSON line per run (raw evidence) ----
    std::printf(
        "{\"bench\":\"chunk_e0_bench\",\"label\":\"%s\","
        "\"module\":\"engine\",\"chunk\":%llu,\"depth\":%llu,"
        "\"workers\":1,\"file_bytes\":%llu,\"chunks\":%llu,"
        "\"total_ns\":%llu,\"bytes_copied\":%llu,"
        "\"read_ops\":%llu,\"write_ops\":%llu,"
        "\"short_writes\":%llu,\"utime_us\":%llu,\"stime_us\":%llu,"
        "\"maxrss_kb\":%ld,\"minflt\":%ld,\"majflt\":%ld,\"ok\":true}\n",
        cfg.label.c_str(), (unsigned long long)cfg.chunk,
        (unsigned long long)cfg.depth, (unsigned long long)cfg.file_bytes,
        (unsigned long long)chunks, (unsigned long long)total_ns,
        (unsigned long long)st.bytes_copied, (unsigned long long)st.read_ops,
        (unsigned long long)st.write_ops, (unsigned long long)st.short_writes,
        (unsigned long long)utime_us, (unsigned long long)stime_us,
        maxrss_kb, minflt, majflt);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    if (cfg.src.empty()) e0_semantic("--src required");
    if (cfg.generate) {
        generate_file(cfg);
        std::printf("generated %s (%llu bytes)\n", cfg.src.c_str(),
                    (unsigned long long)cfg.file_bytes);
        return 0;
    }
    if (cfg.dst.empty()) e0_semantic("--dst required");
    if (!cfg.run) e0_semantic("--run required (or --generate)");
    return run_one(cfg);
}
