// #580 R6 production-shaped A/B runner (WSL2 research-only). Runs the same
// pipeline/cancel shapes as the trigger probe on an UNINSTRUMENTED build of
// the async sources. The arm is fixed at compile time (SLUICE_580_ARM) so each
// arm binary is byte-stable; the runner reports wall/user/sys per phase and a
// full correctness oracle on every read.
//
// Arms: baseline | we2 (cancel scan direction reversed) | candidates added in
// later stages via the same gate. This file never defines the instrumentation
// macro; timings from the instrumented probe are never used from here.

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifndef SLUICE_580_ARM
#define SLUICE_580_ARM baseline
#endif

#define SLUICE_580_STR2(x) #x
#define SLUICE_580_STR(x) SLUICE_580_STR2(x)
#define SLUICE_580_ARM_STRING SLUICE_580_STR(SLUICE_580_ARM)

using namespace sluice;
using sluice::async::AsyncIoContext;
using sluice::async::RequestScope;

namespace {

struct Config {
    std::string scenario = "pipeline";
    std::size_t capacity = 16;
    std::size_t depth = 16;
    std::size_t ops = 20000;
    std::size_t payload = 4096;
    double cancel_ratio = 0.0;
    std::size_t warmup = 0;
    std::string fixture_dir = "/tmp";
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (a == "--scenario") c.scenario = next();
        else if (a == "--capacity") c.capacity = std::stoull(next());
        else if (a == "--depth") c.depth = std::stoull(next());
        else if (a == "--ops") c.ops = std::stoull(next());
        else if (a == "--payload") c.payload = std::stoull(next());
        else if (a == "--cancel-ratio") c.cancel_ratio = std::stod(next());
        else if (a == "--warmup") c.warmup = std::stoull(next());
        else if (a == "--fixture-dir") c.fixture_dir = next();
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); std::exit(2); }
    }
    return c;
}

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

std::string fixture_path(const Config& c, std::uint64_t salt) {
    return c.fixture_dir + "/sluice580_ab_" + std::to_string(salt) + "_" +
           std::to_string(getpid()) + ".bin";
}

Result<File> make_fixture2(const Config& c, std::uint64_t salt) {
    File* out_file_dummy = nullptr;
    (void)out_file_dummy;
    const std::size_t bytes = std::max<std::size_t>(c.payload * 64, 1u << 20);
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::create_new;
    mode.contents = FileInitialContents::truncate;
    auto opened = File::open(fixture_path(c, salt), mode);
    if (!opened.has_value())
        return opened;
    std::vector<std::byte> buffer(c.payload);
    for (std::size_t i = 0; i < c.payload; ++i)
        buffer[i] = static_cast<std::byte>((i * 131u + 7u) & 0xffu);
    std::size_t written = 0;
    while (written < bytes) {
        const std::size_t chunk = std::min(c.payload, bytes - written);
        auto res = blocking::write_all_at(opened.value(), written,
                                          std::span<const std::byte>(buffer.data(), chunk));
        if (!res.has_value() || !res.value().complete())
            return make_unexpected<File>(IoError{IoError::Code::backend_error});
        written += chunk;
    }
    (void)blocking::sync_data(opened.value());
    return opened;
}

bool bytes_match(const std::byte* actual, std::size_t len, std::uint64_t base) {
    for (std::size_t i = 0; i < len; ++i) {
        const auto expected = static_cast<std::byte>(((base + i) * 131u + 7u) & 0xffu);
        if (actual[i] != expected)
            return false;
    }
    return true;
}

struct PhaseResult {
    bool ok = true;
    std::string failure;
    std::size_t completed_ops = 0;
    std::size_t bytes_verified = 0;
    double wall = 0;
    double user = 0;
    double sys = 0;
};

PhaseResult run_pipeline(AsyncIoContext& ctx, RequestScope& scope, const File& file,
                         std::size_t ops_count, const Config& c) {
    PhaseResult out;
    struct Inflight {
        async::ScopeTicket<std::size_t> ticket;
        std::uint64_t offset = 0;
        std::byte* buffer = nullptr;
    };
    std::vector<std::vector<std::byte>> buffers(c.depth, std::vector<std::byte>(c.payload));
    std::vector<Inflight> window;
    std::size_t submitted = 0;
    const std::size_t max_block = std::max<std::size_t>(c.payload * 32, 1u << 20);
    rusage before{};
    getrusage(RUSAGE_SELF, &before);
    const double t0 = now_s();
    while (out.completed_ops < ops_count) {
        while (window.size() < c.depth && submitted < ops_count) {
            const std::size_t slot = submitted % c.depth;
            const std::uint64_t offset = (submitted * c.payload) % max_block;
            async::ReadOp op;
            op.file = async::NativeFileRef{file};
            op.dst = buffers[slot].data();
            op.offset = offset;
            op.len = c.payload;
            auto ticket = scope.submit_read(op);
            if (!ticket.has_value()) {
                out.ok = false;
                out.failure = "submit failed";
                return out;
            }
            window.push_back(Inflight{ticket.value(), offset, buffers[slot].data()});
            ++submitted;
        }
        auto& oldest = window.front();
        auto waited = scope.wait_for(oldest.ticket, std::chrono::milliseconds(5000));
        if (!waited.has_value() || waited.value() != async::ScopeWaitStatus::ready) {
            out.ok = false;
            out.failure = "wait not ready";
            return out;
        }
        auto observed = scope.take(oldest.ticket);
        if (observed.readiness != async::RequestReadiness::ready ||
            !observed.result.has_value() || observed.result.value() != c.payload ||
            !bytes_match(oldest.buffer, c.payload, oldest.offset)) {
            out.ok = false;
            out.failure = "read oracle mismatch";
            return out;
        }
        out.bytes_verified += c.payload;
        ++out.completed_ops;
        window.erase(window.begin());
    }
    rusage after{};
    getrusage(RUSAGE_SELF, &after);
    out.wall = now_s() - t0;
    out.user = double(after.ru_utime.tv_sec - before.ru_utime.tv_sec) +
               double(after.ru_utime.tv_usec - before.ru_utime.tv_usec) * 1e-6;
    out.sys = double(after.ru_stime.tv_sec - before.ru_stime.tv_sec) +
              double(after.ru_stime.tv_usec - before.ru_stime.tv_usec) * 1e-6;
    return out;
}

PhaseResult run_cancel_mix(AsyncIoContext& ctx, RequestScope& scope, const File& file,
                           std::size_t ops_count, const Config& c) {
    PhaseResult out;
    std::vector<std::vector<std::byte>> buffers(c.depth, std::vector<std::byte>(c.payload));
    struct Open {
        async::ScopeTicket<std::size_t> ticket;
        std::uint64_t offset = 0;
        std::byte* buffer = nullptr;
    };
    std::vector<Open> window;
    std::size_t submitted = 0;
    const std::size_t max_block = std::max<std::size_t>(c.payload * 32, 1u << 20);
    rusage before{};
    getrusage(RUSAGE_SELF, &before);
    const double t0 = now_s();
    while (out.completed_ops < ops_count) {
        while (window.size() < c.depth && submitted < ops_count) {
            const std::size_t slot = submitted % c.depth;
            const std::uint64_t offset = (submitted * c.payload) % max_block;
            async::ReadOp op;
            op.file = async::NativeFileRef{file};
            op.dst = buffers[slot].data();
            op.offset = offset;
            op.len = c.payload;
            auto ticket = scope.submit_read(op);
            if (!ticket.has_value()) {
                out.ok = false;
                out.failure = "submit failed";
                return out;
            }
            if (c.cancel_ratio > 0 && (submitted % 1000) < c.cancel_ratio * 1000)
                (void)ctx.cancel(ticket.value().id());
            window.push_back(Open{ticket.value(), offset, buffers[slot].data()});
            ++submitted;
        }
        auto& oldest = window.front();
        auto waited = scope.wait_for(oldest.ticket, std::chrono::milliseconds(5000));
        if (!waited.has_value() || waited.value() != async::ScopeWaitStatus::ready) {
            out.ok = false;
            out.failure = "wait not ready";
            return out;
        }
        auto observed = scope.take(oldest.ticket);
        if (observed.readiness != async::RequestReadiness::ready) {
            out.ok = false;
            out.failure = "take not ready";
            return out;
        }
        if (observed.result.has_value()) {
            if (observed.result.value() != c.payload ||
                !bytes_match(oldest.buffer, c.payload, oldest.offset)) {
                out.ok = false;
                out.failure = "read oracle mismatch";
                return out;
            }
            out.bytes_verified += c.payload;
        }
        ++out.completed_ops;
        window.erase(window.begin());
    }
    rusage after{};
    getrusage(RUSAGE_SELF, &after);
    out.wall = now_s() - t0;
    out.user = double(after.ru_utime.tv_sec - before.ru_utime.tv_sec) +
               double(after.ru_utime.tv_usec - before.ru_utime.tv_usec) * 1e-6;
    out.sys = double(after.ru_stime.tv_sec - before.ru_stime.tv_sec) +
              double(after.ru_stime.tv_usec - before.ru_stime.tv_usec) * 1e-6;
    return out;
}

} // namespace

int main(int argc, char** argv) {
    Config c = parse_args(argc, argv);

    const std::uint64_t salt = static_cast<std::uint64_t>(::time(nullptr));
    auto fixture = make_fixture2(c, salt);
    if (!fixture.has_value()) {
        std::fprintf(stderr, "fixture failed\n");
        return 1;
    }

    File file = std::move(fixture).value();
    sluice::async::UringConfig config;
    config.request_capacity = c.capacity;
    config.queue_depth = static_cast<unsigned>(std::max<std::size_t>(c.capacity, 8));
    auto made = sluice::async::make_uring_context(config, nullptr);
    if (!made.has_value()) {
        std::fprintf(stderr, "uring context failed\n");
        return 1;
    }
    AsyncIoContext ctx = std::move(made).value();
    RequestScope scope(ctx, std::max(c.depth, std::size_t(1)),
                       sluice::async::ScopeCleanupPolicy::drain);

    PhaseResult warm;
    if (c.warmup > 0) {
        if (c.scenario == "cancel")
            warm = run_cancel_mix(ctx, scope, file, c.warmup, c);
        else
            warm = run_pipeline(ctx, scope, file, c.warmup, c);
        if (!warm.ok) {
            std::printf("{\"arm\":\"%s\",\"phase\":\"warmup\",\"ok\":false,\"failure\":\"%s\"}\n",
                        SLUICE_580_ARM_STRING, warm.failure.c_str());
            return 1;
        }
    }

    PhaseResult out;
    if (c.scenario == "cancel")
        out = run_cancel_mix(ctx, scope, file, c.ops, c);
    else
        out = run_pipeline(ctx, scope, file, c.ops, c);
    auto fin = scope.finish();
    if (!fin.has_value()) {
        out.ok = false;
        out.failure = "scope finish failed";
    }
    std::printf(
        "{\"arm\":\"%s\",\"phase\":\"timed\",\"ok\":%s,\"failure\":\"%s\","
        "\"scenario\":\"%s\",\"capacity\":%zu,\"depth\":%zu,\"ops\":%zu,"
        "\"payload\":%zu,\"completed_ops\":%zu,\"bytes_verified\":%zu,"
        "\"wall_s\":%.6f,\"user_s\":%.6f,\"sys_s\":%.6f}\n",
        SLUICE_580_ARM_STRING, out.ok ? "true" : "false", out.failure.c_str(),
        c.scenario.c_str(), c.capacity, c.depth, c.ops, c.payload, out.completed_ops,
        out.bytes_verified, out.wall, out.user, out.sys);
    (void)unlink(fixture_path(c, salt).c_str());
    return out.ok ? 0 : 1;
}
