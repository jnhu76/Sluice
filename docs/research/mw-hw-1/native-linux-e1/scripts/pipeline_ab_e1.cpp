// E1 native-Linux A/B harness (W-02 shape).
//
// Derived from bench/native-linux-m2/pipeline_ab.cpp at master
// 381756370980f75e4beaf47310db4b8b9a99053d. The pipeline loop, phase
// structure and byte oracle are unchanged; the additive JSONL fields are
// rusage user/system CPU, voluntary/involuntary context switches, the
// backend-reported max outstanding depth, the driver window high-water and
// the p95 latency. The same source is compiled against each arm's static
// library; the only per-arm difference is the linked library.
//
// This is not a product benchmark: it is not registered with xmake and is
// not built by `xmake`, `xmake test` or CI.

#include "bench_common.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size))
        return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size);
}
void* operator new(std::size_t size, std::align_val_t align) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    const std::size_t a = static_cast<std::size_t>(align);
    const std::size_t rounded = (size + a - 1) / a * a;
    if (void* p = std::aligned_alloc(a, rounded))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return operator new(size, align);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileOpen;
using sluice::IoError;
using sluice::AsyncStats;
using sluice::async::AsyncIoContext;
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

constexpr std::size_t kFixtureBytes = 64u << 20;
constexpr auto kWaitDeadline = std::chrono::seconds(20);

struct Params {
    std::string arm = "unlabeled";
    std::string backend = "uring";
    std::size_t block = 4096;
    std::size_t depth = 64;
    std::size_t capacity = 64;
    std::size_t scope = 0;
    std::size_t workers = 4;
    std::size_t requests = 20000;
    std::size_t warmup = 2;
    bool any_ready = false;
};

std::string describe(const sluice::IoError& e) {
    return std::string(sluice::to_string(e.code)) + "(errno=" + std::to_string(e.os_errno) + ")";
}

bool write_fixture(const std::string& path, std::string& failure) {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::create_if_missing;
    mode.contents = sluice::FileInitialContents::truncate;
    auto opened = File::open(path, mode);
    if (!opened.has_value()) {
        failure = "create failed: " + describe(opened.error());
        return false;
    }
    File file = std::move(opened).value();
    constexpr std::size_t kChunk = 1u << 20;
    std::vector<std::byte> buffer(kChunk);
    std::size_t written = 0;
    while (written < kFixtureBytes) {
        const std::size_t chunk = std::min(kChunk, kFixtureBytes - written);
        bench::fill_pattern(buffer.data(), chunk, written);
        auto res = sluice::blocking::write_all_at(
            file, written, std::span<const std::byte>(buffer.data(), chunk));
        if (!res.has_value() || !res.value().complete()) {
            failure = "fixture write failed";
            return false;
        }
        written += chunk;
    }
    return sluice::blocking::sync_data(file).has_value();
}

std::optional<AsyncIoContext> make_context(const Params& p, AsyncStats& stats,
                                           std::string& failure) {
    try {
        if (p.backend == "threadpool") {
            ThreadPoolConfig config;
            config.request_capacity = p.capacity;
            config.worker_count = p.workers;
            return sluice::async::make_threadpool_context(config, &stats);
        }
        sluice::async::UringConfig config;
        config.request_capacity = p.capacity;
        config.queue_depth = static_cast<unsigned>(p.capacity);
        auto made = sluice::async::make_uring_context(config, &stats);
        if (!made.has_value()) {
            failure = "io_uring backend not available";
            return std::nullopt;
        }
        return std::move(made).value();
    } catch (const std::exception& e) {
        failure = e.what();
        return std::nullopt;
    }
}

AsyncStats stats_diff(const AsyncStats& after, const AsyncStats& before) {
    AsyncStats d;
    d.submit_calls = after.submit_calls - before.submit_calls;
    d.submitted_ops = after.submitted_ops - before.submitted_ops;
    d.poll_calls = after.poll_calls - before.poll_calls;
    d.wait_calls = after.wait_calls - before.wait_calls;
    d.completed_ops = after.completed_ops - before.completed_ops;
    d.canceled_ops = after.canceled_ops - before.canceled_ops;
    d.completion_errors = after.completion_errors - before.completion_errors;
    d.short_completions = after.short_completions - before.short_completions;
    d.max_outstanding = after.max_outstanding;
    d.queue_full_retries = after.queue_full_retries - before.queue_full_retries;
    d.invalid_state_rejections = after.invalid_state_rejections - before.invalid_state_rejections;
    return d;
}

struct RusageSnapshot {
    double utime_s = 0.0;
    double stime_s = 0.0;
    long nvcsw = 0;
    long nivcsw = 0;
};

RusageSnapshot take_rusage() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
        return RusageSnapshot{};
    RusageSnapshot snap;
    snap.utime_s = static_cast<double>(usage.ru_utime.tv_sec) +
                   static_cast<double>(usage.ru_utime.tv_usec) * 1e-6;
    snap.stime_s = static_cast<double>(usage.ru_stime.tv_sec) +
                   static_cast<double>(usage.ru_stime.tv_usec) * 1e-6;
    snap.nvcsw = usage.ru_nvcsw;
    snap.nivcsw = usage.ru_nivcsw;
    return snap;
}

struct PhaseOutcome {
    bool ok = false;
    std::string failure;
    double wall_s = 0.0;
    double cpu_s = 0.0;
    double utime_s = 0.0;
    double stime_s = 0.0;
    long nvcsw = 0;
    long nivcsw = 0;
    std::size_t ops = 0;
    std::size_t bytes = 0;
    std::size_t window_high_water = 0;
    std::uint64_t allocations = 0;
    AsyncStats stats;
    std::vector<double> latencies_ns;
};

PhaseOutcome run_phase(const Params& p, const std::string& path, bool verify, bool instrument) {
    PhaseOutcome out;
    auto opened = File::open(path);
    if (!opened.has_value()) {
        out.failure = "open failed: " + describe(opened.error());
        return out;
    }
    File file = std::move(opened).value();
    AsyncStats stats;
    std::string context_failure;
    auto context = make_context(p, stats, context_failure);
    if (!context.has_value()) {
        out.failure = "NOT_RUN_WITH_REASON: " + context_failure;
        return out;
    }
    AsyncIoContext ctx = std::move(*context);
    const std::size_t scope_capacity = p.scope != 0 ? p.scope : p.depth;
    const std::size_t blocks = (kFixtureBytes - p.block) / p.block;
    std::vector<std::vector<std::byte>> buffers(p.depth, std::vector<std::byte>(p.block));
    for (std::size_t i = 0; i < p.depth; ++i)
        bench::fill_pattern(buffers[i].data(), p.block, 0);

    struct Inflight {
        ScopeTicket<std::size_t> ticket;
        std::size_t slot = 0;
        std::uint64_t offset = 0;
        double submitted_ns = 0.0;
    };

    RequestScope scope(ctx, scope_capacity, ScopeCleanupPolicy::drain);
    std::vector<Inflight> window;
    window.reserve(p.depth);
    std::vector<std::size_t> free_slots(p.depth);
    for (std::size_t i = 0; i < p.depth; ++i)
        free_slots[i] = i;

    const double cpu_before = bench::process_cpu_seconds();
    const RusageSnapshot rusage_before = take_rusage();
    const std::uint64_t alloc_before = g_allocations.load(std::memory_order_relaxed);
    const bench::TimePoint start = bench::now();
    std::size_t submitted = 0;
    std::size_t completed = 0;
    std::size_t local_high_water = 0;
    while (completed < p.requests) {
        while (window.size() < p.depth && submitted < p.requests) {
            const std::size_t slot = free_slots.back();
            free_slots.pop_back();
            const std::uint64_t offset =
                static_cast<std::uint64_t>(submitted % blocks) * p.block;
            ReadOp op;
            op.file = NativeFileRef(file);
            op.dst = buffers[slot].data();
            op.len = p.block;
            op.offset = offset;
            auto ticket = scope.submit_read(op);
            if (!ticket.has_value()) {
                free_slots.push_back(slot);
                if (ticket.error().code == IoError::Code::would_block)
                    break;
                out.failure = "submit failed: " + describe(ticket.error());
                return out;
            }
            window.push_back(Inflight{ticket.value(), slot, offset,
                                      instrument ? bench::now_ns() : 0.0});
            ++submitted;
        }
        if (window.size() > local_high_water)
            local_high_water = window.size();
        if (window.empty()) {
            out.failure = "pipeline stalled at operation " + std::to_string(completed);
            return out;
        }
        std::size_t victim = window.size();
        if (p.any_ready) {
            for (std::size_t i = 0; i < window.size(); ++i) {
                if (ctx.lookup(window[i].ticket.id()) == RequestReadiness::ready) {
                    victim = i;
                    break;
                }
            }
        }
        RequestObservation<std::size_t> observed;
        if (victim == window.size()) {
            Inflight& oldest = window.front();
            auto waited = scope.wait_for(oldest.ticket, kWaitDeadline);
            if (!waited.has_value()) {
                out.failure = "wait failed: " + describe(waited.error());
                return out;
            }
            if (waited.value() != ScopeWaitStatus::ready) {
                out.failure = "wait outcome was not ready at " + std::to_string(completed);
                return out;
            }
            observed = scope.take(oldest.ticket);
            victim = 0;
        } else {
            observed = scope.take(window[victim].ticket);
        }
        if (observed.readiness != RequestReadiness::ready) {
            out.failure = "take did not observe ready at " + std::to_string(completed);
            return out;
        }
        Inflight& entry = window[victim];
        if (!observed.result.has_value()) {
            out.failure = "operation failed: " + describe(observed.result.error());
            return out;
        }
        if (observed.result.value() != p.block) {
            out.failure = "short read: " + std::to_string(observed.result.value()) + " of " +
                          std::to_string(p.block);
            return out;
        }
        if (instrument)
            out.latencies_ns.push_back(bench::now_ns() - entry.submitted_ns);
        if (verify) {
            const auto mismatch = bench::verify_pattern(buffers[entry.slot].data(), p.block,
                                                        entry.offset);
            if (!mismatch.ok) {
                out.failure = "byte mismatch at absolute " +
                              std::to_string(mismatch.first_bad_position);
                return out;
            }
        }
        out.bytes += observed.result.value();
        ++out.ops;
        ++completed;
        free_slots.push_back(entry.slot);
        window.erase(window.begin() + static_cast<std::ptrdiff_t>(victim));
    }
    const bench::TimePoint end = bench::now();
    const double cpu_after = bench::process_cpu_seconds();
    const RusageSnapshot rusage_after = take_rusage();
    const std::uint64_t alloc_after = g_allocations.load(std::memory_order_relaxed);
    out.wall_s = bench::seconds_between(start, end);
    out.cpu_s = cpu_after - cpu_before;
    out.utime_s = rusage_after.utime_s - rusage_before.utime_s;
    out.stime_s = rusage_after.stime_s - rusage_before.stime_s;
    out.nvcsw = rusage_after.nvcsw - rusage_before.nvcsw;
    out.nivcsw = rusage_after.nivcsw - rusage_before.nivcsw;
    out.allocations = alloc_after - alloc_before;
    out.window_high_water = local_high_water;
    auto finished = scope.finish();
    if (!finished.has_value()) {
        out.failure = "scope finish failed";
        return out;
    }
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    if (!outcome.has_value()) {
        out.failure = "shutdown failed";
        return out;
    }
    out.stats = stats_diff(stats, AsyncStats{});
    out.ok = true;
    return out;
}

void emit(const Params& p, const char* phase, const PhaseOutcome& o) {
    std::vector<double> sorted_latencies = o.latencies_ns;
    std::sort(sorted_latencies.begin(), sorted_latencies.end());
    const double ops_per_sec = o.wall_s > 0 ? static_cast<double>(o.ops) / o.wall_s : 0.0;
    const double cpu_ns_per_op =
        o.ops > 0 ? (o.cpu_s * 1e9) / static_cast<double>(o.ops) : 0.0;
    const double utime_ns_per_op =
        o.ops > 0 ? (o.utime_s * 1e9) / static_cast<double>(o.ops) : 0.0;
    const double stime_ns_per_op =
        o.ops > 0 ? (o.stime_s * 1e9) / static_cast<double>(o.ops) : 0.0;
    std::printf("{\"arm\":\"%s\",\"backend\":\"%s\",\"block\":%zu,\"depth\":%zu,"
                "\"capacity\":%zu,\"scope\":%zu,\"workers\":%zu,\"requests\":%zu,"
                "\"phase\":\"%s\",\"ok\":%s,\"wall_s\":%.9f,\"ops_per_sec\":%.3f,"
                "\"cpu_s\":%.9f,\"cpu_ns_per_op\":%.3f,"
                "\"utime_ns_per_op\":%.3f,\"stime_ns_per_op\":%.3f,"
                "\"nvcsw\":%ld,\"nivcsw\":%ld,"
                "\"max_outstanding\":%llu,\"window_high_water\":%zu,"
                "\"ops\":%zu,\"bytes\":%zu,"
                "\"allocations\":%llu,\"p50_ns\":%.1f,\"p95_ns\":%.1f,\"p90_ns\":%.1f,\"p99_ns\":%.1f,"
                "\"submit_calls\":%llu,\"poll_calls\":%llu,\"completed_ops\":%llu,"
                "\"completion_errors\":%llu,\"canceled_ops\":%llu,"
                "\"invalid_state\":%llu,\"queue_full_retries\":%llu,\"failure\":\"%s\"}\n",
                p.arm.c_str(), p.backend.c_str(), p.block, p.depth, p.capacity,
                p.scope != 0 ? p.scope : p.depth, p.workers, p.requests, phase,
                o.ok ? "true" : "false", o.wall_s, ops_per_sec, o.cpu_s, cpu_ns_per_op,
                utime_ns_per_op, stime_ns_per_op,
                o.nvcsw, o.nivcsw,
                static_cast<unsigned long long>(o.stats.max_outstanding),
                o.window_high_water,
                o.ops, o.bytes, static_cast<unsigned long long>(o.allocations),
                sorted_latencies.empty() ? 0.0 : bench::quantile_sorted(sorted_latencies, 0.50),
                sorted_latencies.empty() ? 0.0 : bench::quantile_sorted(sorted_latencies, 0.95),
                sorted_latencies.empty() ? 0.0 : bench::quantile_sorted(sorted_latencies, 0.90),
                sorted_latencies.empty() ? 0.0 : bench::quantile_sorted(sorted_latencies, 0.99),
                static_cast<unsigned long long>(o.stats.submit_calls),
                static_cast<unsigned long long>(o.stats.poll_calls),
                static_cast<unsigned long long>(o.stats.completed_ops),
                static_cast<unsigned long long>(o.stats.completion_errors),
                static_cast<unsigned long long>(o.stats.canceled_ops),
                static_cast<unsigned long long>(o.stats.invalid_state_rejections),
                static_cast<unsigned long long>(o.stats.queue_full_retries),
                o.failure.c_str());
    std::fflush(stdout);
}

std::size_t parse_size(const std::string& value) {
    return static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
}

} // namespace

int main(int argc, char** argv) {
    Params p;
    std::string fixture;
    bool create_fixture = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* key) -> std::optional<std::string> {
            const std::string prefix = std::string("--") + key + "=";
            if (arg.rfind(prefix, 0) == 0)
                return arg.substr(prefix.size());
            return std::nullopt;
        };
        if (auto v = value("arm")) p.arm = *v;
        else if (auto v = value("backend")) p.backend = *v;
        else if (auto v = value("block")) p.block = parse_size(*v);
        else if (auto v = value("depth")) p.depth = parse_size(*v);
        else if (auto v = value("capacity")) p.capacity = parse_size(*v);
        else if (auto v = value("scope")) p.scope = parse_size(*v);
        else if (auto v = value("workers")) p.workers = parse_size(*v);
        else if (auto v = value("requests")) p.requests = parse_size(*v);
        else if (auto v = value("warmup")) p.warmup = parse_size(*v);
        else if (auto v = value("fixture")) fixture = *v;
        else if (auto v = value("anyready")) p.any_ready = (*v != "0");
        else if (arg == "--create-fixture") create_fixture = true;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (fixture.empty()) {
        std::fprintf(stderr, "--fixture=<path> is required\n");
        return 2;
    }
    if (create_fixture) {
        std::string failure;
        if (!write_fixture(fixture, failure)) {
            std::fprintf(stderr, "fixture creation failed: %s\n", failure.c_str());
            return 1;
        }
        std::printf("fixture created: %s\n", fixture.c_str());
        return 0;
    }
    if (p.depth == 0 || p.capacity == 0 || p.requests == 0 || p.block == 0) {
        std::fprintf(stderr, "depth, capacity, requests and block must be nonzero\n");
        return 2;
    }
    if (p.depth > p.capacity) {
        std::fprintf(stderr, "depth must not exceed capacity\n");
        return 2;
    }
    if (p.block > kFixtureBytes / 2) {
        std::fprintf(stderr, "block exceeds fixture\n");
        return 2;
    }

    const bench::OracleSelfTest oracle_gate = bench::oracle_self_test();
    if (!oracle_gate.ok) {
        std::fprintf(stderr, "oracle self-test failed: %s\n", oracle_gate.failure.c_str());
        return 1;
    }

    for (std::size_t i = 0; i < p.warmup; ++i) {
        const PhaseOutcome warm = run_phase(p, fixture, false, false);
        if (!warm.ok) {
            emit(p, "warmup", warm);
            std::fprintf(stderr, "warmup failed: %s\n", warm.failure.c_str());
            return 1;
        }
    }
    const PhaseOutcome timed = run_phase(p, fixture, false, false);
    emit(p, "timed", timed);
    if (!timed.ok) {
        std::fprintf(stderr, "timed phase failed: %s\n", timed.failure.c_str());
        return 1;
    }
    const PhaseOutcome oracle = run_phase(p, fixture, true, false);
    emit(p, "oracle", oracle);
    if (!oracle.ok) {
        std::fprintf(stderr, "oracle phase failed: %s\n", oracle.failure.c_str());
        return 1;
    }
    const PhaseOutcome latency = run_phase(p, fixture, false, true);
    emit(p, "latency", latency);
    if (!latency.ok) {
        std::fprintf(stderr, "latency phase failed: %s\n", latency.failure.c_str());
        return 1;
    }
    return 0;
}
