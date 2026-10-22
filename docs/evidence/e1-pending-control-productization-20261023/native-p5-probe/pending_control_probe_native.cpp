// Real-kernel P>0 reachability probe against the product HEAD library.
//
// Congestion-storm workload on a real io_uring ring (no submit seam):
// instant-completing tmpfs reads accumulate in the CQ faster than they are
// reaped while the oldest kernel-visible op is canceled every cycle. A
// monitor thread samples the single-critical-section pending-control
// snapshot continuously: any sticky state would appear as derived_count > 0.
// Output is one JSON line per run.

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace sluice::async;

struct ProbeReport {
    std::atomic<std::uint64_t> samples{0};
    std::atomic<std::uint64_t> p_positive_samples{0};
    std::atomic<std::uint64_t> parity_violations{0};
    std::atomic<std::uint64_t> control_sqe_observations{0};
    std::atomic<std::uint64_t> cancel_attempts{0};
    std::atomic<std::uint64_t> cancel_requested{0};
    std::atomic<std::uint64_t> cancel_other{0};
    std::atomic<std::size_t> p_max{0};
};

std::size_t parse_size(const char* v) {
    return static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
}

} // namespace

int main(int argc, char** argv) {
    using namespace sluice;
    using namespace sluice::async;
    ::alarm(600);
    std::size_t requests = 20000;
    std::size_t window_size = 24;
    unsigned capacity = 32;
    unsigned queue_depth = 8;
    unsigned run = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* key) -> const char* {
            const std::string prefix = std::string("--") + key + "=";
            return arg.rfind(prefix, 0) == 0 ? arg.c_str() + prefix.size() : nullptr;
        };
        if (const char* v = value("requests")) requests = parse_size(v);
        else if (const char* v = value("window")) window_size = parse_size(v);
        else if (const char* v = value("capacity")) capacity = static_cast<unsigned>(parse_size(v));
        else if (const char* v = value("depth")) queue_depth = static_cast<unsigned>(parse_size(v));
        else if (const char* v = value("run")) run = static_cast<unsigned>(parse_size(v));
        else if (const char* v = value("fixture")) {
            // fixture handled below
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }

    const char* fixture = "/tmp/e1-prod-probe-fixture";
    FILE* made = ::fopen(fixture, "wb");
    if (made == nullptr)
        return 1;
    std::byte content[4096];
    std::memset(content, 0x33, sizeof(content));
    for (std::size_t i = 0; i < 64; ++i) {
        if (::fwrite(content, 1, sizeof(content), made) != sizeof(content))
            return 1;
    }
    ::fclose(made);

    try {
        auto owned = std::make_unique<UringAsyncBackend>(UringConfig{capacity, queue_depth});
        UringAsyncBackend* backend = owned.get();
        auto ctx =
            std::make_unique<AsyncIoContext>(sluice::async::detail::make_context(std::move(owned)));

        ProbeReport report;
        std::atomic<bool> stop_monitor{false};
        std::thread monitor([&] {
            while (!stop_monitor.load(std::memory_order_acquire)) {
                const auto snap = backend->pending_control_snapshot_for_test();
                report.samples.fetch_add(1, std::memory_order_relaxed);
                if (snap.derived_count != snap.authoritative_pending)
                    report.parity_violations.fetch_add(1, std::memory_order_relaxed);
                if (snap.derived_count > 0) {
                    report.p_positive_samples.fetch_add(1, std::memory_order_relaxed);
                    std::size_t prev = report.p_max.load(std::memory_order_relaxed);
                    while (snap.derived_count > prev &&
                           !report.p_max.compare_exchange_weak(prev, snap.derived_count,
                                                               std::memory_order_relaxed)) {
                    }
                }
                if (backend->live_control_sqes_for_test() > 0)
                    report.control_sqe_observations.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });

        auto opened = sluice::File::open(fixture);
        if (!opened.has_value())
            return 1;
        sluice::File file = std::move(opened).value();
        std::vector<std::byte> buffer(4096);
        std::deque<Request<std::size_t>> window;
        std::size_t submitted = 0;
        std::size_t settled = 0;
        bool failed = false;
        const char* failure = "";
        const auto start = std::chrono::steady_clock::now();
        while (settled < requests && !failed) {
            while (window.size() < window_size && submitted < requests) {
                ReadOp op;
                op.file = NativeFileRef(file);
                op.dst = buffer.data();
                op.len = buffer.size();
                op.offset = 0;
                auto made_request = ctx->submit_read(op);
                if (!made_request.has_value()) {
                    if (made_request.error().code == IoError::Code::would_block)
                        break;
                    failure = "submit rejected";
                    failed = true;
                    break;
                }
                window.push_back(std::move(made_request).value());
                ++submitted;
            }
            if (failed)
                break;
            if (window.empty()) {
                failure = "storm stalled";
                failed = true;
                break;
            }
            report.cancel_attempts.fetch_add(1, std::memory_order_relaxed);
            const auto seen = ctx->cancel(window.front().id());
            if (seen.has_value() && seen.value() == CancelDisposition::requested)
                report.cancel_requested.fetch_add(1, std::memory_order_relaxed);
            else
                report.cancel_other.fetch_add(1, std::memory_order_relaxed);
            for (int guard = 0; guard < 1000000 && !window.front().ready(); ++guard) {
                (void)ctx->poll();
            }
            if (!window.front().ready()) {
                failure = "canceled request never settled";
                failed = true;
                break;
            }
            (void)window.front().take_result();
            ++settled;
            window.pop_front();
        }
        const auto end = std::chrono::steady_clock::now();
        const double wall_s = std::chrono::duration<double>(end - start).count();

        stop_monitor.store(true, std::memory_order_release);
        monitor.join();
        const auto shutdown = ctx->shutdown(ShutdownPolicy::drain);
        if (!shutdown.has_value()) {
            failed = true;
            failure = "shutdown failed";
        }

        std::printf(
            "{\"run\":%u,\"ok\":%s,\"requests\":%zu,\"settled\":%zu,\"wall_s\":%.6f,"
            "\"samples\":%llu,\"p_positive_samples\":%llu,\"parity_violations\":%llu,"
            "\"control_sqe_observations\":%llu,\"p_max\":%zu,"
            "\"cancel_attempts\":%llu,\"cancel_requested\":%llu,\"cancel_other\":%llu,"
            "\"failure\":\"%s\"}\n",
            run, failed ? "false" : "true", requests, settled, wall_s,
            static_cast<unsigned long long>(report.samples.load()),
            static_cast<unsigned long long>(report.p_positive_samples.load()),
            static_cast<unsigned long long>(report.parity_violations.load()),
            static_cast<unsigned long long>(report.control_sqe_observations.load()),
            report.p_max.load(),
            static_cast<unsigned long long>(report.cancel_attempts.load()),
            static_cast<unsigned long long>(report.cancel_requested.load()),
            static_cast<unsigned long long>(report.cancel_other.load()), failure);
        std::fflush(stdout);
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "exception: %s\n", e.what());
        return 1;
    }
}
