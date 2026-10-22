// DP-0 data-movement microbenchmark (research arms only; not a production target).
// One case per process invocation so an external perf stat wraps exactly one case.
//
// Usage: bench_dp0_movement <case> <iterations>
// Cases:
//   zop      zero-length reads, uring D1 (publication-heavy: inline publication)
//   cons4k   4 KiB reads, uring D1 (result-consumption + slot-recycle heavy)
//   thr64    64 KiB reads, uring D16 (throughput control, kernel-bound)
//   write4k  4 KiB writes, uring D4
//   finfo    file_info requests, uring D1 (metadata-required path)
//   constp   4 KiB reads, threadpool W4 D4
//   w01ctl   blocking read_at loop (negative control, must be arm-independent)

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <chrono>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <span>
#include <string>
#include <vector>

#include <unistd.h>

using namespace sluice;
using async::AsyncIoContext;
using async::Request;
using async::RequestReadiness;
using async::ReadOp;
using async::WriteOp;
using async::FileInfoOp;
using async::NativeFileRef;
using async::UringConfig;
using async::ThreadPoolConfig;
using async::ShutdownPolicy;
using async::ShutdownOutcome;
using async::CancelDisposition;

namespace {

constexpr std::size_t kFixtureBytes = 64u << 20;

std::string tmp_dir() {
    if (const char* v = std::getenv("SLUICE_BENCH_TMP_DIR")) {
        return v;
    }
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::string(home) + "/tmp" : std::string("/tmp");
}

bool fill_pattern(std::byte* dst, std::size_t n, std::uint64_t seed) {
    std::uint64_t x = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        dst[i] = static_cast<std::byte>(x >> 33);
    }
    return true;
}

bool write_fixture(const std::string& path) {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::create_if_missing;
    mode.contents = FileInitialContents::truncate;
    auto opened = File::open(path, mode);
    if (!opened.has_value()) {
        return false;
    }
    File file = std::move(opened).value();
    std::vector<std::byte> buffer(1u << 20);
    for (std::size_t written = 0; written < kFixtureBytes; written += buffer.size()) {
        fill_pattern(buffer.data(), buffer.size(), written);
        auto res = blocking::write_all_at(file, written,
                                          std::span<const std::byte>(buffer.data(), buffer.size()));
        if (!res.has_value() || !res.value().complete()) {
            return false;
        }
    }
    return blocking::sync_data(file).has_value();
}

struct CaseResult {
    double wall_s = 0.0;
    std::uint64_t ops = 0;
    bool ok = false;
    std::string failure;
};

std::optional<AsyncIoContext> make_uring(std::size_t depth) {
    UringConfig config;
    config.request_capacity = depth;
    config.queue_depth = static_cast<unsigned>(depth);
    auto made = make_uring_context(config);
    if (!made.has_value()) {
        return std::nullopt;
    }
    return std::move(made).value();
}

CaseResult run_case(const std::string& which, std::size_t iterations) {
    CaseResult out;
    const std::string path = tmp_dir() + "/dp0-movement.bin";

    if (which == "w01ctl") {
        auto opened = File::open(path);
        if (!opened.has_value()) {
            out.failure = "fixture open failed";
            return out;
        }
        File file = std::move(opened).value();
        std::vector<std::byte> buffer(4096);
        std::uint64_t bytes = 0;
        const auto warm = iterations / 10 + 1;
        const auto start = std::chrono::steady_clock::now();
        double warm_wall = 0.0;
        for (std::size_t i = 0; i < warm + iterations; ++i) {
            if (i == warm) {
                warm_wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                bytes = 0;
            }
            const std::uint64_t offset = static_cast<std::uint64_t>(i % 4096) * 4096;
            auto r = blocking::read_at(file, offset, std::span<std::byte>(buffer));
            if (!r.has_value()) {
                out.failure = "read_at failed";
                return out;
            }
            bytes += r.value();
        }
        out.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() - warm_wall;
        out.ops = bytes;
        out.ok = true;
        return out;
    }

    if (which == "constp") {
        ThreadPoolConfig config;
        config.request_capacity = 8;
        config.worker_count = 4;
        AsyncIoContext ctx = make_threadpool_context(config);
        auto opened = File::open(path);
        if (!opened.has_value()) {
            out.failure = "fixture open failed";
            return out;
        }
        File file = std::move(opened).value();
        std::vector<std::byte> buffer(4096);
        fill_pattern(buffer.data(), buffer.size(), 0);
        std::uint64_t bytes = 0;
        const auto warm = iterations / 10 + 1;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < warm + iterations; ++i) {
            if (i == warm) {
                out.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                bytes = 0;
            }
            ReadOp op;
            op.file = NativeFileRef(file);
            op.dst = buffer.data();
            op.len = 4096;
            op.offset = static_cast<std::uint64_t>(i % 4096) * 4096;
            auto r = ctx.submit_read(op);
            if (!r.has_value()) {
                out.failure = "submit failed";
                return out;
            }
            Request<std::size_t> request = std::move(r).value();
            while (request.try_result().readiness != RequestReadiness::ready) {
                if (!ctx.poll().has_value()) {
                    out.failure = "poll failed";
                    return out;
                }
            }
            auto taken = request.take_result();
            if (!taken.result.has_value()) {
                out.failure = "read failed";
                return out;
            }
            bytes += taken.result.value();
        }
        out.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() - out.wall_s;
        out.ops = bytes;
        out.ok = true;
        (void)ctx.shutdown(ShutdownPolicy::drain);
        return out;
    }

    auto made = make_uring(which == "thr64" ? 16 : (which == "write4k" ? 4 : 1));
    if (!made.has_value()) {
        out.failure = "uring context failed";
        return out;
    }
    AsyncIoContext ctx = std::move(*made);
    FileOpen open_mode;
    if (which == "write4k") {
        open_mode.access = FileAccess::read_write;
    }
    auto opened = File::open(path, open_mode);
    if (!opened.has_value()) {
        out.failure = "fixture open failed";
        return out;
    }
    File file = std::move(opened).value();

    const std::size_t block = which == "thr64" ? 65536 : 4096;
    const std::size_t depth = which == "thr64" ? 16 : (which == "write4k" ? 4 : 1);
    std::vector<std::vector<std::byte>> buffers(depth, std::vector<std::byte>(block));
    for (auto& b : buffers) {
        fill_pattern(b.data(), b.size(), 0);
    }

    std::uint64_t bytes = 0;
    const auto warm = iterations / 10 + 1;
    const auto start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < warm + iterations; ++i) {
        if (i == warm) {
            out.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            bytes = 0;
        }
        const std::uint64_t offset = static_cast<std::uint64_t>(i % 4096) * block;

        if (which == "zop") {
            ReadOp op;
            op.file = NativeFileRef(file);
            op.dst = nullptr;
            op.len = 0;
            op.offset = 0;
            auto r = ctx.submit_read(op);
            if (!r.has_value()) {
                out.failure = "zero-op submit failed";
                return out;
            }
            Request<std::size_t> request = std::move(r).value();
            auto taken = request.take_result();
            if (!taken.result.has_value()) {
                out.failure = "zero-op result failed";
                return out;
            }
            if (!ctx.poll().has_value()) {
                out.failure = "poll failed";
                return out;
            }
            bytes += 1;
        } else if (which == "finfo") {
            FileInfoOp op;
            op.file = NativeFileRef(file);
            auto r = ctx.submit_file_info(op);
            if (!r.has_value()) {
                out.failure = "file_info submit failed";
                return out;
            }
            Request<FileInfo> request = std::move(r).value();
            while (request.try_result().readiness != RequestReadiness::ready) {
                if (!ctx.poll().has_value()) {
                    out.failure = "poll failed";
                    return out;
                }
            }
            auto taken = request.take_result();
            if (!taken.result.has_value()) {
                out.failure = "file_info failed";
                return out;
            }
            bytes += 1;
        } else if (which == "write4k") {
            const std::size_t slot = i % depth;
            WriteOp op;
            op.file = NativeFileRef(file);
            op.src = buffers[slot].data();
            op.len = block;
            op.offset = offset;
            auto r = ctx.submit_write(op);
            if (!r.has_value()) {
                out.failure = "write submit failed";
                return out;
            }
            Request<std::size_t> request = std::move(r).value();
            while (request.try_result().readiness != RequestReadiness::ready) {
                if (!ctx.poll().has_value()) {
                    out.failure = "poll failed";
                    return out;
                }
            }
            auto taken = request.take_result();
            if (!taken.result.has_value()) {
                out.failure = "write failed";
                return out;
            }
            bytes += taken.result.value();
        } else if (which == "thr64") {
            const std::size_t slot = i % depth;
            ReadOp op;
            op.file = NativeFileRef(file);
            op.dst = buffers[slot].data();
            op.len = block;
            op.offset = offset;
            auto r = ctx.submit_read(op);
            if (!r.has_value()) {
                out.failure = "read submit failed";
                return out;
            }
            Request<std::size_t> request = std::move(r).value();
            while (request.try_result().readiness != RequestReadiness::ready) {
                if (!ctx.poll().has_value()) {
                    out.failure = "poll failed";
                    return out;
                }
            }
            auto taken = request.take_result();
            if (!taken.result.has_value()) {
                out.failure = "read failed";
                return out;
            }
            bytes += taken.result.value();
        } else {  // cons4k
            ReadOp op;
            op.file = NativeFileRef(file);
            op.dst = buffers[0].data();
            op.len = block;
            op.offset = offset;
            auto r = ctx.submit_read(op);
            if (!r.has_value()) {
                out.failure = "read submit failed";
                return out;
            }
            Request<std::size_t> request = std::move(r).value();
            while (request.try_result().readiness != RequestReadiness::ready) {
                if (!ctx.poll().has_value()) {
                    out.failure = "poll failed";
                    return out;
                }
            }
            auto taken = request.take_result();
            if (!taken.result.has_value()) {
                out.failure = "read failed";
                return out;
            }
            bytes += taken.result.value();
        }
    }
    out.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() - out.wall_s;
    out.ops = bytes;
    out.ok = true;
    (void)ctx.shutdown(ShutdownPolicy::drain);
    return out;
}

}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <case> <iterations>\n", argv[0]);
        return 2;
    }
    const std::string which = argv[1];
    const std::size_t iterations = std::strtoul(argv[2], nullptr, 10);
    const std::string fixture = tmp_dir() + "/dp0-movement.bin";
    bool need_fixture = true;
    if (std::getenv("SLUICE_DP0_REUSE_FIXTURE") != nullptr) {
        struct stat st {};
        if (::stat(fixture.c_str(), &st) == 0 &&
            static_cast<std::uint64_t>(st.st_size) == kFixtureBytes) {
            need_fixture = false;
        }
    }
    if (need_fixture && !write_fixture(fixture)) {
        std::fprintf(stderr, "fixture creation failed\n");
        return 1;
    }
    const CaseResult r = run_case(which, iterations);
    if (!r.ok) {
        std::fprintf(stderr, "case failed: %s\n", r.failure.c_str());
        return 1;
    }
    std::printf("{\"case\":\"%s\",\"iters\":%zu,\"wall_s\":%.9f,\"ops\":%llu,\"ops_per_s\":%.1f}\n",
                which.c_str(), iterations, r.wall_s,
                static_cast<unsigned long long>(r.ops),
                static_cast<double>(r.ops) / r.wall_s);
    return 0;
}
