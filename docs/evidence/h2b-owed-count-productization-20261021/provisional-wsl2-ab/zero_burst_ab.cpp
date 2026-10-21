// One-shot provisional A/B driver for the K>0 zero-op burst shape (issue #564).
// Same measurement and oracle discipline as bench/native-linux-m2/pipeline_ab.cpp
// but with zero-length reads so every in-flight request stands as an owed
// delivery event. Not committed to the product tree; the recorded results are.
#include "bench_common.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <algorithm>
#include <atomic>
#include <new>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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

constexpr auto kWaitDeadline = std::chrono::seconds(20);

struct Params {
    std::string arm = "unlabeled";
    std::size_t depth = 64;
    std::size_t capacity = 1024;
    std::size_t scope = 0;
    std::size_t requests = 20000;
    std::size_t warmup = 2;
};

std::string describe(const sluice::IoError& e) {
    return std::string(sluice::to_string(e.code)) + "(errno=" + std::to_string(e.os_errno) + ")";
}

std::optional<AsyncIoContext> make_context(const Params& p, AsyncStats& stats,
                                           std::string& failure) {
    try {
        sluice::async::UringConfig config;
        config.request_capacity = p.capacity;
        config.queue_depth = 64;
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
    return d;
}

struct PhaseOutcome {
    bool ok = false;
    std::string failure;
    double wall_s = 0.0;
    double cpu_s = 0.0;
    std::size_t ops = 0;
    std::uint64_t allocations = 0;
    AsyncStats stats;
};

PhaseOutcome run_phase(const Params& p, const std::string& path) {
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

    RequestScope scope(ctx, p.scope != 0 ? p.scope : p.depth, ScopeCleanupPolicy::drain);
    std::vector<ScopeTicket<std::size_t>> window;
    window.reserve(p.depth);

    // Zero-length reads publish inline at submission, so readiness alone
    // drives no progress pass and the owed delivery would never be swept.
    // Each batch submits a standing window of owed events and then drives
    // bounded passes until the backend has swept the whole batch.
    const double cpu_before = bench::process_cpu_seconds();
    const std::uint64_t alloc_before = g_allocations.load(std::memory_order_relaxed);
    const bench::TimePoint start = bench::now();
    std::size_t completed = 0;
    while (completed < p.requests) {
        const std::size_t batch = std::min(p.depth, p.requests - completed);
        for (std::size_t i = 0; i < batch; ++i) {
            ReadOp op;
            op.file = NativeFileRef(file);
            op.dst = nullptr;
            op.len = 0;
            op.offset = 0;
            auto ticket = scope.submit_read(op);
            if (!ticket.has_value()) {
                out.failure = "submit failed at " + std::to_string(completed) + ": " +
                              describe(ticket.error());
                return out;
            }
            window.push_back(std::move(ticket).value());
        }
        std::size_t swept = 0;
        while (swept < batch) {
            auto published = ctx.poll();
            if (!published.has_value()) {
                out.failure = "poll failed: " + describe(published.error());
                return out;
            }
            swept += published.value();
        }
        for (auto& ticket : window) {
            RequestObservation<std::size_t> observed = scope.take(ticket);
            if (observed.readiness != RequestReadiness::ready || !observed.result.has_value() ||
                observed.result.value() != 0) {
                out.failure = "zero-op outcome wrong at " + std::to_string(completed);
                return out;
            }
            ++completed;
        }
        window.clear();
    }
    const bench::TimePoint end = bench::now();
    const double cpu_after = bench::process_cpu_seconds();
    const std::uint64_t alloc_after = g_allocations.load(std::memory_order_relaxed);
    out.wall_s = bench::seconds_between(start, end);
    out.cpu_s = cpu_after - cpu_before;
    out.ops = completed;
    out.allocations = alloc_after - alloc_before;
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
    const double ops_per_sec = o.wall_s > 0 ? static_cast<double>(o.ops) / o.wall_s : 0.0;
    const double cpu_ns_per_op = o.ops > 0 ? (o.cpu_s * 1e9) / static_cast<double>(o.ops) : 0.0;
    std::printf("{\"arm\":\"%s\",\"backend\":\"uring\",\"block\":0,\"depth\":%zu,"
                "\"capacity\":%zu,\"scope\":%zu,\"workers\":0,\"requests\":%zu,"
                "\"phase\":\"%s\",\"ok\":%s,\"wall_s\":%.9f,\"ops_per_sec\":%.3f,"
                "\"cpu_s\":%.9f,\"cpu_ns_per_op\":%.3f,\"ops\":%zu,\"bytes\":0,"
                "\"allocations\":%llu,\"p50_ns\":0.0,\"p90_ns\":0.0,\"p99_ns\":0.0,"
                "\"submit_calls\":%llu,\"poll_calls\":%llu,\"completed_ops\":%llu,"
                "\"completion_errors\":0,\"canceled_ops\":0,"
                "\"invalid_state\":0,\"queue_full_retries\":0,\"failure\":\"%s\"}\n",
                p.arm.c_str(), p.depth, p.capacity, p.scope != 0 ? p.scope : p.depth,
                p.requests, phase, o.ok ? "true" : "false", o.wall_s, ops_per_sec, o.cpu_s,
                cpu_ns_per_op, o.ops, static_cast<unsigned long long>(o.allocations),
                static_cast<unsigned long long>(o.stats.submit_calls),
                static_cast<unsigned long long>(o.stats.poll_calls),
                static_cast<unsigned long long>(o.stats.completed_ops),
                o.failure.c_str());
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    Params p;
    std::string fixture;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* key) -> std::optional<std::string> {
            const std::string prefix = std::string("--") + key + "=";
            if (arg.rfind(prefix, 0) == 0)
                return arg.substr(prefix.size());
            return std::nullopt;
        };
        if (auto v = value("arm")) p.arm = *v;
        else if (auto v = value("depth")) p.depth = static_cast<std::size_t>(std::strtoull(v->c_str(), nullptr, 10));
        else if (auto v = value("capacity")) p.capacity = static_cast<std::size_t>(std::strtoull(v->c_str(), nullptr, 10));
        else if (auto v = value("scope")) p.scope = static_cast<std::size_t>(std::strtoull(v->c_str(), nullptr, 10));
        else if (auto v = value("requests")) p.requests = static_cast<std::size_t>(std::strtoull(v->c_str(), nullptr, 10));
        else if (auto v = value("warmup")) p.warmup = static_cast<std::size_t>(std::strtoull(v->c_str(), nullptr, 10));
        else if (auto v = value("fixture")) fixture = *v;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (fixture.empty()) {
        std::fprintf(stderr, "--fixture=<path> is required\n");
        return 2;
    }
    for (std::size_t i = 0; i < p.warmup; ++i) {
        const PhaseOutcome warm = run_phase(p, fixture);
        if (!warm.ok) {
            emit(p, "warmup", warm);
            return 1;
        }
    }
    const PhaseOutcome timed = run_phase(p, fixture);
    emit(p, "timed", timed);
    if (!timed.ok)
        return 1;
    return 0;
}
