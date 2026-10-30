// #576 research harness (research workspace only; never part of the product).
// Modes: mkfixture | pipeline | zeroburst | mixed | idlepass
// Built twice: linked against the pristine baseline libs (timing) and the
// instrumented libs (census). sluice576_* resolve weakly to zeros when the
// instrumented census is not linked.

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileOpen;
using sluice::IoError;
using sluice::async::AsyncIoContext;
using sluice::AsyncStats;
using sluice::async::NativeFileRef;
using sluice::async::ReadOp;
using sluice::async::RequestObservation;
using sluice::async::RequestReadiness;
using sluice::async::RequestScope;
using sluice::async::ScopeCleanupPolicy;
using sluice::async::ScopeTicket;
using sluice::async::ScopeWaitStatus;
using sluice::async::ShutdownPolicy;
using sluice::async::ThreadPoolConfig;

constexpr std::size_t kPatternPeriod = 4096;
constexpr std::size_t kFixtureBytes = 64u << 20;
constexpr auto kWaitDeadline = std::chrono::seconds(60);

enum class OpMode { pipeline, zero_all, mixed };

const std::byte* pattern_table() {
    static std::byte table[kPatternPeriod];
    static std::atomic<bool> ready{false};
    if (!ready.load(std::memory_order_acquire)) {
        for (std::size_t i = 0; i < kPatternPeriod; ++i)
            table[i] = static_cast<std::byte>((i * 131 + 7) & 0xFF);
        ready.store(true, std::memory_order_release);
    }
    return table;
}

void fill_pattern(std::byte* dst, std::size_t len, std::uint64_t base) noexcept {
    const std::byte* t = pattern_table();
    std::size_t offset = static_cast<std::size_t>(base % kPatternPeriod);
    std::size_t done = 0;
    while (done < len) {
        const std::size_t chunk = std::min(kPatternPeriod - offset, len - done);
        std::memcpy(dst + done, t + offset, chunk);
        done += chunk;
        offset = 0;
    }
}

bool verify_pattern(const std::byte* src, std::size_t len, std::uint64_t base) noexcept {
    const std::byte* t = pattern_table();
    std::size_t offset = static_cast<std::size_t>(base % kPatternPeriod);
    std::size_t done = 0;
    while (done < len) {
        const std::size_t chunk = std::min(kPatternPeriod - offset, len - done);
        if (std::memcmp(src + done, t + offset, chunk) != 0)
            return false;
        done += chunk;
        offset = 0;
    }
    return true;
}

double now_ns() noexcept {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

double process_cpu_seconds() noexcept {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_utime.tv_sec) +
           static_cast<double>(ru.ru_utime.tv_usec) * 1e-6 +
           static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) * 1e-6;
}

struct Args {
    std::string mode;
    std::size_t capacity = 16;
    std::size_t depth = 16;
    std::size_t workers = 4;
    std::size_t block = 4096;
    std::size_t ops = 20000;
    std::size_t reps = 1;
    bool verify = false;
    std::string fixture = "/tmp/sluice576/fixture.bin";
    std::size_t zero_ratio = 1; // mixed: zero-length submits per one nonzero submit
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--mode") a.mode = value();
        else if (arg == "--capacity") a.capacity = std::stoul(value());
        else if (arg == "--depth") a.depth = std::stoul(value());
        else if (arg == "--workers") a.workers = std::stoul(value());
        else if (arg == "--block") a.block = std::stoul(value());
        else if (arg == "--ops") a.ops = std::stoul(value());
        else if (arg == "--reps") a.reps = std::stoul(value());
        else if (arg == "--zero-ratio") a.zero_ratio = std::stoul(value());
        else if (arg == "--fixture") a.fixture = value();
        else if (arg == "--verify") a.verify = true;
        else { std::fprintf(stderr, "unknown arg %s\n", arg.c_str()); std::exit(2); }
    }
    return a;
}

FileOpen rw_open_existing() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::open_existing;
    return mode;
}

bool write_fixture(const std::string& path) {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::create_if_missing;
    mode.contents = sluice::FileInitialContents::truncate;
    auto opened = File::open(path, mode);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    constexpr std::size_t kChunk = 1u << 20;
    std::vector<std::byte> buffer(kChunk);
    std::size_t written = 0;
    while (written < kFixtureBytes) {
        const std::size_t chunk = std::min(kChunk, kFixtureBytes - written);
        fill_pattern(buffer.data(), chunk, written);
        auto res = sluice::blocking::write_all_at(
            file, written, std::span<const std::byte>(buffer.data(), chunk));
        if (!res.has_value() || !res.value().complete())
            return false;
        written += chunk;
    }
    return sluice::blocking::sync_data(file).has_value();
}

std::optional<AsyncIoContext> make_context(const Args& a, AsyncStats* stats) {
    ThreadPoolConfig config;
    config.request_capacity = a.capacity;
    config.worker_count = a.workers;
    try {
        return sluice::async::make_threadpool_context(config, stats);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

struct PhaseReport {
    bool ok = true;
    std::string failure;
    double wall_s = 0.0;
    double cpu_s = 0.0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    std::uint64_t zero_ops = 0;
    std::uint64_t nonzero_ops = 0;
    std::uint64_t waits = 0;
    std::uint64_t would_block = 0;
};

struct CensusPod {
    std::uint64_t v[16];
};

extern "C" {
// Weak fallbacks: zero when linked against the pristine baseline libs.
__attribute__((weak)) void sluice576_reset(void) noexcept {}
__attribute__((weak)) void sluice576_snapshot(std::uint64_t* out, int n) noexcept {
    for (int i = 0; i < n; ++i)
        out[i] = 0;
}
}

CensusPod census_snapshot() {
    CensusPod pod{};
    sluice576_snapshot(pod.v, 16);
    return pod;
}

void print_phase(const Args& a, std::size_t rep, const PhaseReport& r, const CensusPod& before,
                 const CensusPod& after) {
    auto delta = [&](int i) { return after.v[i] - before.v[i]; };
    const double denom = r.ops ? static_cast<double>(r.ops) : 1.0;
    std::printf(
        "R576 PHASE rep=%zu mode=%s C=%zu D=%zu W=%zu block=%zu ops=%llu zero=%llu nonzero=%llu "
        "wall_s=%.6f cpu_s=%.6f ops_per_s=%.1f ok=%d fail=%s waits=%llu would_block=%llu "
        "s1_passes=%llu s1_slots=%llu s1_k_sum=%llu s1_k_nonzero=%llu pub_entry_sum=%llu "
        "s2_checks=%llu s2_slots=%llu s2_found=%llu s3_checks=%llu s3_slots=%llu "
        "s4_checks=%llu s4_slots=%llu "
        "w1_resets=%llu w1_saw_true=%llu w2_sets=%llu w3_clears=%llu "
        "passes_per_op=%.5f s1_slots_per_op=%.1f k0_frac=%.4f\n",
        rep, a.mode.c_str(), a.capacity, a.depth, a.workers, a.block,
        static_cast<unsigned long long>(r.ops), static_cast<unsigned long long>(r.zero_ops),
        static_cast<unsigned long long>(r.nonzero_ops), r.wall_s, r.cpu_s,
        r.wall_s > 0 ? static_cast<double>(r.ops) / r.wall_s : 0.0, r.ok ? 1 : 0,
        r.failure.c_str(), static_cast<unsigned long long>(r.waits),
        static_cast<unsigned long long>(r.would_block),
        static_cast<unsigned long long>(delta(0)), static_cast<unsigned long long>(delta(1)),
        static_cast<unsigned long long>(delta(2)), static_cast<unsigned long long>(delta(3)),
        static_cast<unsigned long long>(delta(4)), static_cast<unsigned long long>(delta(5)),
        static_cast<unsigned long long>(delta(6)), static_cast<unsigned long long>(delta(7)),
        static_cast<unsigned long long>(delta(8)), static_cast<unsigned long long>(delta(9)),
        static_cast<unsigned long long>(delta(10)), static_cast<unsigned long long>(delta(11)),
        static_cast<unsigned long long>(delta(12)), static_cast<unsigned long long>(delta(13)),
        static_cast<unsigned long long>(delta(14)), static_cast<unsigned long long>(delta(15)),
        static_cast<double>(delta(0)) / denom, static_cast<double>(delta(1)) / denom,
        delta(0) > 0 ? 1.0 - static_cast<double>(delta(3)) / static_cast<double>(delta(0)) : 0.0);
    std::fflush(stdout);
}

struct Inflight {
    ScopeTicket<std::size_t> ticket;
    std::size_t slot = 0;
    std::uint64_t offset = 0;
    std::size_t expect_bytes = 0;
};

PhaseReport run_window_phase(const Args& a, OpMode opmode) {
    PhaseReport result;
    auto opened = File::open(a.fixture, rw_open_existing());
    if (!opened.has_value()) {
        result.ok = false;
        result.failure = "open failed";
        return result;
    }
    File file = std::move(opened).value();

    AsyncStats stats;
    auto context = make_context(a, &stats);
    if (!context.has_value()) {
        result.ok = false;
        result.failure = "context failed";
        return result;
    }
    AsyncIoContext ctx = std::move(*context);

    const std::size_t blocks = (kFixtureBytes - a.block) / a.block;
    std::vector<std::vector<std::byte>> buffers(a.depth, std::vector<std::byte>(a.block));
    for (std::size_t i = 0; i < a.depth; ++i)
        fill_pattern(buffers[i].data(), a.block, 0);

    RequestScope scope(ctx, a.depth, ScopeCleanupPolicy::drain);
    std::deque<Inflight> window;
    std::size_t submitted = 0;
    std::size_t completed = 0;
    std::vector<std::size_t> free_slots(a.depth);
    for (std::size_t i = 0; i < a.depth; ++i)
        free_slots[i] = i;

    const double start = now_ns();
    while (completed < a.ops) {
        while (window.size() < a.depth && submitted < a.ops) {
            const std::size_t slot = free_slots.back();
            free_slots.pop_back();
            const bool zero =
                opmode == OpMode::zero_all ||
                (opmode == OpMode::mixed && (submitted % (a.zero_ratio + 1)) != 0);
            ReadOp op;
            op.file = NativeFileRef(file);
            op.dst = buffers[slot].data();
            op.len = zero ? 0 : a.block;
            op.offset = static_cast<std::uint64_t>(submitted % blocks) * a.block;
            auto ticket = scope.submit_read(op);
            if (!ticket.has_value()) {
                if (ticket.error().code == IoError::Code::would_block) {
                    // Zero-op owed events pin one control ref each until a
                    // progress pass delivers them; a backpressure-aware
                    // driver polls, then retries admission.
                    ++result.would_block;
                    bool admitted = false;
                    for (int retry = 0; retry < 10000; ++retry) {
                        auto pass = ctx.poll_progress();
                        if (!pass.has_value()) {
                            result.ok = false;
                            result.failure = "poll failed";
                            return result;
                        }
                        auto again = scope.submit_read(op);
                        if (again.has_value()) {
                            ticket = std::move(again);
                            admitted = true;
                            break;
                        }
                        if (again.error().code != IoError::Code::would_block) {
                            result.ok = false;
                            result.failure = "submit failed";
                            return result;
                        }
                    }
                    if (!admitted) {
                        free_slots.push_back(slot);
                        break;
                    }
                } else {
                    result.ok = false;
                    result.failure = "submit failed";
                    return result;
                }
            }
            window.push_back(Inflight{ticket.value(), slot, op.offset, zero ? 0 : a.block});
            ++submitted;
        }
        if (window.empty()) {
            result.ok = false;
            result.failure = "stalled";
            return result;
        }
        Inflight& oldest = window.front();
        ++result.waits;
        auto waited = scope.wait_for(oldest.ticket, kWaitDeadline);
        if (!waited.has_value() || waited.value() != ScopeWaitStatus::ready) {
            result.ok = false;
            result.failure = "wait not ready";
            return result;
        }
        RequestObservation<std::size_t> observed = scope.take(oldest.ticket);
        if (observed.readiness != RequestReadiness::ready || !observed.result.has_value()) {
            result.ok = false;
            result.failure = "take failed";
            return result;
        }
        if (observed.result.value() != oldest.expect_bytes) {
            result.ok = false;
            result.failure = "length oracle mismatch: got " +
                             std::to_string(observed.result.value()) + " want " +
                             std::to_string(oldest.expect_bytes);
            return result;
        }
        if (a.verify && oldest.expect_bytes != 0 &&
            !verify_pattern(buffers[oldest.slot].data(), oldest.expect_bytes, oldest.offset)) {
            result.ok = false;
            result.failure = "byte mismatch";
            return result;
        }
        if (oldest.expect_bytes == 0)
            ++result.zero_ops;
        else
            ++result.nonzero_ops;
        result.bytes += observed.result.value();
        ++result.ops;
        ++completed;
        free_slots.push_back(oldest.slot);
        window.pop_front();
    }
    result.wall_s = (now_ns() - start) * 1e-9;

    if (!scope.finish().has_value()) {
        result.ok = false;
        result.failure = "scope finish failed";
        return result;
    }
    if (!ctx.shutdown(ShutdownPolicy::drain).has_value()) {
        result.ok = false;
        result.failure = "shutdown failed";
        return result;
    }
    return result;
}

PhaseReport run_idlepass(const Args& a) {
    PhaseReport result;
    AsyncStats stats;
    auto context = make_context(a, &stats);
    if (!context.has_value()) {
        result.ok = false;
        result.failure = "context failed";
        return result;
    }
    AsyncIoContext ctx = std::move(*context);
    sluice576_reset();
    const CensusPod before = census_snapshot();
    const double start = now_ns();
    std::size_t passes = 0;
    for (std::size_t i = 0; i < a.ops; ++i) {
        auto pass = ctx.poll_progress();
        if (!pass.has_value()) {
            result.ok = false;
            result.failure = "poll failed";
            return result;
        }
        if (pass.value().completed != 0) {
            result.ok = false;
            result.failure = "idle pass completed work";
            return result;
        }
        ++passes;
    }
    const double end = now_ns();
    result.wall_s = (end - start) * 1e-9;
    result.ops = passes;
    const CensusPod after = census_snapshot();
    const std::uint64_t s1 = after.v[1] - before.v[1];
    const std::uint64_t s2 = after.v[6] - before.v[6];
    std::printf("R576 IDLE C=%zu passes=%zu ns_per_pass=%.2f s1_slots_per_pass=%llu "
                "s2_slots_per_pass=%.1f ok=%d\n",
                a.capacity, passes, result.wall_s * 1e9 / static_cast<double>(passes),
                static_cast<unsigned long long>(s1),
                static_cast<double>(s2) / static_cast<double>(passes), result.ok ? 1 : 0);
    std::fflush(stdout);
    return result;
}

} // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    if (a.mode.empty()) {
        std::fprintf(stderr,
                     "usage: r576 --mode <mkfixture|pipeline|zeroburst|mixed|idlepass> ...\n");
        return 2;
    }
    if (a.mode == "mkfixture")
        return write_fixture(a.fixture) ? 0 : 1;
    if (a.mode == "idlepass")
        return run_idlepass(a).ok ? 0 : 1;

    OpMode opmode = OpMode::pipeline;
    if (a.mode == "zeroburst")
        opmode = OpMode::zero_all;
    else if (a.mode == "mixed")
        opmode = OpMode::mixed;
    else if (a.mode != "pipeline") {
        std::fprintf(stderr, "unknown mode %s\n", a.mode.c_str());
        return 2;
    }

    int failed = 0;
    for (std::size_t rep = 0; rep < a.reps; ++rep) {
        sluice576_reset();
        const CensusPod before = census_snapshot();
        const double cpu_before = process_cpu_seconds();
        PhaseReport r = run_window_phase(a, opmode);
        r.cpu_s = process_cpu_seconds() - cpu_before;
        const CensusPod after = census_snapshot();
        print_phase(a, rep, r, before, after);
        if (!r.ok)
            failed = 1;
    }
    return failed;
}
