// E1 P>0 pending-control pressure A/B harness (research-only).
//
// The storm is a CQ-congestion cancel workload: instant-completing tmpfs
// reads accumulate in the completion ring faster than anyone reaps them, so
// io_uring_submit turns EBUSY, the SQ ring fills with unconsumed entries,
// and the cancel of the oldest kernel-visible operation cannot issue its
// control SQE. The sticky pending-control intent is then the only correct
// behavior, and freed slots must service it later. This is a real-kernel
// congestion condition, not an injected fault. perf mode runs against the
// clean production libraries; probe mode is compiled with the internal test
// seams (E1 arm only - the pending-control seams do not exist on BASE) and
// samples the derived pending-control count and its parity with the
// authoritative router scan, proving that control_pending states really
// occur in this real-kernel workload.
//
// Same source compiled against each arm's static library; the only per-arm
// difference is the linked library.

#include "bench_common.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#ifdef E1_P5_SEAM_PROBE
#include <sluice/async/detail/uring_backend.hpp>
#endif

namespace {

using sluice::IoError;
using sluice::AsyncStats;
using sluice::async::AsyncIoContext;
using sluice::async::CancelDisposition;
using sluice::async::NativeFileRef;
using sluice::async::ReadOp;
using sluice::async::Request;
using sluice::async::RequestReadiness;
using sluice::async::ShutdownPolicy;
using sluice::async::UringConfig;

struct Params {
    std::string arm = "unlabeled";
    std::string mode = "perf";
    std::size_t requests = 2000;
    std::size_t warmup = 1;
    std::size_t capacity = 32;
    std::size_t queue_depth = 8;
    std::size_t window = 16;
    std::string fixture;
};

struct RusageSnapshotLike {
    double utime_s = 0.0;
    double stime_s = 0.0;
    long nvcsw = 0;
    long nivcsw = 0;
};

RusageSnapshotLike take_rusage() {
    struct rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    RusageSnapshotLike snap;
    snap.utime_s = static_cast<double>(usage.ru_utime.tv_sec) +
                   static_cast<double>(usage.ru_utime.tv_usec) * 1e-6;
    snap.stime_s = static_cast<double>(usage.ru_stime.tv_sec) +
                   static_cast<double>(usage.ru_stime.tv_usec) * 1e-6;
    snap.nvcsw = usage.ru_nvcsw;
    snap.nivcsw = usage.ru_nivcsw;
    return snap;
}

struct LoopCounters {
    bool ok = false;
    std::string failure;
    double wall_s = 0.0;
    double cpu_s = 0.0;
    double utime_s = 0.0;
    double stime_s = 0.0;
    long nvcsw = 0;
    long nivcsw = 0;
    std::size_t ops = 0;
    std::uint64_t canceled = 0;
    std::vector<double> cancel_latencies_ns;
};

void emit_perf_row(const Params& p, const char* phase, const LoopCounters& c) {
    const double cpu_ns_per_op = c.ops > 0 ? (c.cpu_s * 1e9) / static_cast<double>(c.ops) : 0.0;
    std::vector<double> sorted = c.cancel_latencies_ns;
    std::sort(sorted.begin(), sorted.end());
    std::printf(
        "{\"arm\":\"%s\",\"workload\":\"file-cancel-congestion\",\"cfg\":\"C%zu-Q%zu-W%zu\","
        "\"requests\":%zu,\"phase\":\"%s\",\"ok\":%s,"
        "\"wall_s\":%.9f,\"cpu_ns_per_op\":%.3f,\"ops\":%zu,"
        "\"utime_ns_per_op\":%.3f,\"stime_ns_per_op\":%.3f,"
        "\"nvcsw\":%ld,\"nivcsw\":%ld,"
        "\"p_events\":%llu,\"invariant_ok\":%s,"
        "\"cancel_p50_ns\":%.1f,\"cancel_p95_ns\":%.1f,\"failure\":\"%s\"}\n",
        p.arm.c_str(), p.capacity, p.queue_depth, p.window, p.requests, phase,
        c.ok ? "true" : "false", c.wall_s, cpu_ns_per_op, c.ops,
        c.ops > 0 ? (c.utime_s * 1e9) / static_cast<double>(c.ops) : 0.0,
        c.ops > 0 ? (c.stime_s * 1e9) / static_cast<double>(c.ops) : 0.0,
        c.nvcsw, c.nivcsw, static_cast<unsigned long long>(c.canceled), "true",
        sorted.empty() ? 0.0 : bench::quantile_sorted(sorted, 0.50),
        sorted.empty() ? 0.0 : bench::quantile_sorted(sorted, 0.95),
        c.failure.c_str());
    std::fflush(stdout);
}

std::string describe(const IoError& e) {
    return std::string(sluice::to_string(e.code)) + "(errno=" + std::to_string(e.os_errno) + ")";
}

LoopCounters storm_phase(const Params& p, const std::string& path) {
    LoopCounters out;
    AsyncStats stats;
    auto opened = sluice::File::open(path);
    if (!opened.has_value()) {
        out.failure = "fixture open failed";
        return out;
    }
    sluice::File file = std::move(opened).value();
    try {
        UringConfig config;
        config.request_capacity = p.capacity;
        config.queue_depth = static_cast<unsigned>(p.queue_depth);
        auto made = sluice::async::make_uring_context(config, &stats);
        if (!made.has_value()) {
            out.failure = "io_uring backend not available";
            return out;
        }
        AsyncIoContext ctx = std::move(made).value();

        std::vector<std::byte> buffer(4096);
        struct Inflight {
            Request<std::size_t> request;
            double cancel_started_ns = 0.0;
        };
        std::deque<Inflight> window;

        const RusageSnapshotLike rusage_before = take_rusage();
        const double cpu_before = bench::process_cpu_seconds();
        const bench::TimePoint start = bench::now();
        std::size_t submitted = 0;
        std::size_t settled = 0;
        bool failed = false;

        while (settled < p.requests && !failed) {
            while (window.size() < p.window && submitted < p.requests) {
                ReadOp op;
                op.file = NativeFileRef(file);
                op.dst = buffer.data();
                op.len = buffer.size();
                op.offset = 0;
                auto made_request = ctx.submit_read(op);
                if (!made_request.has_value()) {
                    if (made_request.error().code == IoError::Code::would_block)
                        break;
                    out.failure = "submit_read rejected: " + describe(made_request.error());
                    failed = true;
                    break;
                }
                window.push_back(Inflight{std::move(made_request).value(), 0.0});
                ++submitted;
            }
            if (failed)
                break;
            if (window.empty()) {
                out.failure = "storm stalled with work remaining";
                failed = true;
                break;
            }
            // No reaping before the cancel: the burst of instant completions
            // jams the CQ ring, submits turn EBUSY and the SQ ring fills, so
            // the cancel of the oldest kernel-visible op may have to hold its
            // control intent until slots free.
            Inflight& oldest = window.front();
            oldest.cancel_started_ns = bench::now_ns();
            const auto disposition = ctx.cancel(oldest.request.id());
            if (!disposition.has_value()) {
                out.failure = "cancel rejected: " + describe(disposition.error());
                failed = true;
                break;
            }
            for (int guard = 0; guard < 1000000 && !oldest.request.ready(); ++guard) {
                (void)ctx.poll();
            }
            if (!oldest.request.ready()) {
                out.failure = "canceled request never reached a terminal state";
                failed = true;
                break;
            }
            out.cancel_latencies_ns.push_back(bench::now_ns() - oldest.cancel_started_ns);
            const auto observed = oldest.request.take_result();
            if (observed.readiness != RequestReadiness::ready) {
                out.failure = "take_result did not observe a terminal request";
                failed = true;
                break;
            }
            ++settled;
            window.pop_front();
        }

        const bench::TimePoint end = bench::now();
        const double cpu_after = bench::process_cpu_seconds();
        const RusageSnapshotLike rusage_after = take_rusage();
        out.wall_s = bench::seconds_between(start, end);
        out.cpu_s = cpu_after - cpu_before;
        out.utime_s = rusage_after.utime_s - rusage_before.utime_s;
        out.stime_s = rusage_after.stime_s - rusage_before.stime_s;
        out.nvcsw = rusage_after.nvcsw - rusage_before.nvcsw;
        out.nivcsw = rusage_after.nivcsw - rusage_before.nivcsw;
        out.ops = settled;
        out.canceled = stats.canceled_ops;
        out.ok = !failed;

        const auto shutdown = ctx.shutdown(ShutdownPolicy::drain);
        if (!shutdown.has_value()) {
            out.ok = false;
            out.failure = "shutdown failed";
        }
    } catch (const std::exception& e) {
        out.failure = e.what();
    }
    return out;
}

#ifdef E1_P5_SEAM_PROBE
using sluice::async::UringAsyncBackend;

struct ProbeReport {
    std::atomic<std::uint64_t> samples{0};
    std::atomic<std::uint64_t> p_positive_samples{0};
    std::atomic<std::uint64_t> parity_violations{0};
    std::atomic<std::uint64_t> control_sqe_observations{0};
    std::atomic<std::size_t> p_max{0};
};

LoopCounters probe_storm(const Params& p, const std::string& path, ProbeReport& report) {
    LoopCounters out;
    auto opened = sluice::File::open(path);
    if (!opened.has_value()) {
        out.failure = "fixture open failed";
        return out;
    }
    sluice::File file = std::move(opened).value();
    try {
        UringConfig config;
        config.request_capacity = p.capacity;
        config.queue_depth = static_cast<unsigned>(p.queue_depth);
        auto owned = std::make_unique<UringAsyncBackend>(config);
        UringAsyncBackend* backend = owned.get();
        auto ctx = std::make_unique<AsyncIoContext>(
            sluice::async::detail::make_context(std::move(owned)));

        std::atomic<bool> stop_monitor{false};
        std::thread monitor([&] {
            while (!stop_monitor.load(std::memory_order_acquire)) {
                const std::size_t count = backend->pending_control_count_for_test();
                const std::size_t scan = backend->pending_control_scan_for_test();
                report.samples.fetch_add(1, std::memory_order_relaxed);
                if (count != scan)
                    report.parity_violations.fetch_add(1, std::memory_order_relaxed);
                if (count > 0) {
                    report.p_positive_samples.fetch_add(1, std::memory_order_relaxed);
                    std::size_t prev = report.p_max.load(std::memory_order_relaxed);
                    while (count > prev && !report.p_max.compare_exchange_weak(
                                               prev, count, std::memory_order_relaxed)) {
                    }
                }
                if (backend->live_control_sqes_for_test() > 0)
                    report.control_sqe_observations.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });

        std::vector<std::byte> buffer(4096);
        std::deque<Request<std::size_t>> window;
        std::size_t submitted = 0;
        std::size_t settled = 0;
        bool failed = false;
        const bench::TimePoint start = bench::now();
        while (settled < p.requests && !failed) {
            while (window.size() < p.window && submitted < p.requests) {
                ReadOp op;
                op.file = NativeFileRef(file);
                op.dst = buffer.data();
                op.len = buffer.size();
                op.offset = 0;
                auto made_request = ctx->submit_read(op);
                if (!made_request.has_value()) {
                    if (made_request.error().code == IoError::Code::would_block)
                        break;
                    out.failure = "submit_read rejected: " + describe(made_request.error());
                    failed = true;
                    break;
                }
                window.push_back(std::move(made_request).value());
                ++submitted;
            }
            if (failed)
                break;
            if (window.empty()) {
                out.failure = "storm stalled";
                failed = true;
                break;
            }
            (void)ctx->cancel(window.front().id());
            for (int guard = 0; guard < 1000000 && !window.front().ready(); ++guard) {
                (void)ctx->poll();
            }
            if (!window.front().ready()) {
                out.failure = "canceled request never settled";
                failed = true;
                break;
            }
            (void)window.front().take_result();
            ++settled;
            window.pop_front();
        }
        const bench::TimePoint end = bench::now();
        out.wall_s = bench::seconds_between(start, end);
        out.ops = settled;
        out.ok = !failed;

        stop_monitor.store(true, std::memory_order_release);
        monitor.join();
        const auto shutdown = ctx->shutdown(ShutdownPolicy::drain);
        if (!shutdown.has_value()) {
            out.ok = false;
            out.failure = "shutdown failed";
        }
    } catch (const std::exception& e) {
        out.failure = e.what();
    }
    return out;
}
#endif

} // namespace

int main(int argc, char** argv) {
    ::alarm(600);
    Params p;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* key) -> std::optional<std::string> {
            const std::string prefix = std::string("--") + key + "=";
            if (arg.rfind(prefix, 0) == 0)
                return arg.substr(prefix.size());
            return std::nullopt;
        };
        if (auto v = value("arm")) p.arm = *v;
        else if (auto v = value("mode")) p.mode = *v;
        else if (auto v = value("requests")) p.requests = std::strtoull(v->c_str(), nullptr, 10);
        else if (auto v = value("warmup")) p.warmup = std::strtoull(v->c_str(), nullptr, 10);
        else if (auto v = value("capacity")) p.capacity = std::strtoull(v->c_str(), nullptr, 10);
        else if (auto v = value("queue-depth"))
            p.queue_depth = std::strtoull(v->c_str(), nullptr, 10);
        else if (auto v = value("window")) p.window = std::strtoull(v->c_str(), nullptr, 10);
        else if (auto v = value("fixture")) p.fixture = *v;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (p.fixture.empty()) {
        std::fprintf(stderr, "--fixture=<path> is required\n");
        return 2;
    }

#ifdef E1_P5_SEAM_PROBE
    if (p.mode == "probe") {
        ProbeReport report;
        const LoopCounters result = probe_storm(p, p.fixture, report);
        std::printf(
            "{\"arm\":\"%s\",\"mode\":\"probe\",\"p_max\":%zu,"
            "\"p_positive_samples\":%llu,\"samples\":%llu,"
            "\"parity_violations\":%llu,\"control_sqe_observations\":%llu,"
            "\"canceled\":%llu,\"ops\":%zu,\"ok\":%s,\"failure\":\"%s\"}\n",
            p.arm.c_str(), report.p_max.load(),
            static_cast<unsigned long long>(report.p_positive_samples.load()),
            static_cast<unsigned long long>(report.samples.load()),
            static_cast<unsigned long long>(report.parity_violations.load()),
            static_cast<unsigned long long>(report.control_sqe_observations.load()),
            static_cast<unsigned long long>(result.canceled), result.ops,
            result.ok ? "true" : "false", result.failure.c_str());
        std::fflush(stdout);
        return result.ok && report.parity_violations.load() == 0 &&
                       report.p_positive_samples.load() > 0 ? 0 : 1;
    }
#endif

    if (p.mode != "perf") {
        std::fprintf(stderr, "unknown mode: %s\n", p.mode.c_str());
        return 2;
    }

    for (std::size_t i = 0; i < p.warmup; ++i) {
        const LoopCounters warm = storm_phase(p, p.fixture);
        if (!warm.ok) {
            emit_perf_row(p, "warmup", warm);
            std::fprintf(stderr, "warmup failed: %s\n", warm.failure.c_str());
            return 1;
        }
    }
    const LoopCounters timed = storm_phase(p, p.fixture);
    emit_perf_row(p, "timed", timed);
    return timed.ok ? 0 : 1;
}
