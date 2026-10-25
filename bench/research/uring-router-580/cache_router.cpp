// #580 R9 cache-locality harness (research-only). Measures the CQE router
// lookup's sensitivity to application cache pressure on two execution shapes:
// a W-02-shaped pipeline driven through RequestScope on one owner thread, and
// a replica of the W-03 external event loop (submitter thread + driver thread
// that claims the progress owner and parks in epoll on the progress
// notification fd — the same public-API structure as bench_w03_external_loop,
// which cannot itself carry the interference seam without modification).
//
// The interference workload is a deterministic sequential sweep over an
// independent working-set buffer (one 8-byte load per 64-byte line, serially
// dependent), interleaved between completions and metered separately from the
// timed I/O loop: its seconds are reported on their own and net_wall_s
// (pipeline shape only, where the same thread serializes both) subtracts
// them so cache-pressure overhead is never attributed to the sweep itself.
//
// Builds: cache_ab / cache_ab_aa_{alpha,beta} (uninstrumented timing),
// cache_ab_flush (adds SLUICE_580_CLFLUSH_ROUTER: every cookie lookup starts
// with the router array evicted — a mechanism sensitivity upper bound, never
// a representative application), cache_probe (adds
// SLUICE_URING_LOOKUP_INSTRUMENT: lookup counts/distributions only; its
// timings are never performance evidence).

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <pthread.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <ctime>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if defined(SLUICE_URING_LOOKUP_INSTRUMENT)
#include "uring_internal.hpp"
#endif

using namespace sluice;
using sluice::async::AsyncIoContext;
using sluice::async::RequestScope;

namespace {

struct Config {
    std::string scenario = "pipeline";
    std::size_t capacity = 0;
    std::size_t depth = 64;
    std::size_t ops = 40000;
    std::size_t payload = 4096;
    std::size_t warmup = 2000;
    std::size_t ws_bytes = 0;
    std::size_t ws_bytes_per_op = 4096;
    std::string interference_side = "submitter";
    std::string pin_mode = "none";
    int cpu_a = 2;
    int cpu_b = 3;
    std::string label = "run";
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
        else if (a == "--warmup") c.warmup = std::stoull(next());
        else if (a == "--ws-bytes") c.ws_bytes = std::stoull(next());
        else if (a == "--ws-bytes-per-op") c.ws_bytes_per_op = std::stoull(next());
        else if (a == "--interference-side") c.interference_side = next();
        else if (a == "--pin-mode") c.pin_mode = next();
        else if (a == "--cpu-a") c.cpu_a = std::stoi(next());
        else if (a == "--cpu-b") c.cpu_b = std::stoi(next());
        else if (a == "--label") c.label = next();
        else if (a == "--fixture-dir") c.fixture_dir = next();
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); std::exit(2); }
    }
    return c;
}

void pin_self_to(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::fprintf(stderr, "pin to cpu %d failed\n", cpu);
        std::exit(3);
    }
}

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

struct Interference {
    std::vector<std::uint64_t> lines;
    std::size_t cursor = 0;
    std::uint64_t sink = 0;
    double seconds = 0.0;

    explicit Interference(std::size_t ws_bytes) : lines(ws_bytes / 64) {
        std::uint64_t x = 0x243F6A8885A308D3ULL;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27; x *= 0x94d049bb133111ebULL;
            x ^= x >> 31;
            lines[i] = x;
        }
    }

    bool active() const { return !lines.empty(); }

    void touch(std::size_t bytes) {
        if (lines.empty())
            return;
        const std::size_t n = lines.size();
        const std::size_t count = bytes / 64;
        const double t0 = now_s();
        std::uint64_t acc = sink;
        std::size_t c = cursor;
        for (std::size_t i = 0; i < count; ++i) {
            acc += lines[c];
            if (++c == n)
                c = 0;
        }
        sink = acc;
        cursor = c;
        seconds += now_s() - t0;
    }
};

std::string fixture_path(const Config& c, std::uint64_t salt) {
    return c.fixture_dir + "/sluice580_cache_" + std::to_string(salt) + "_" +
           std::to_string(getpid()) + ".bin";
}

Result<File> make_fixture(const Config& c, std::uint64_t salt) {
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
    double interf = 0;
    std::uint64_t notifications = 0;
    std::uint64_t empty_wakes = 0;
    std::uint64_t passes = 0;
};

PhaseResult run_pipeline(AsyncIoContext& ctx, RequestScope& scope, const File& file,
                         std::size_t ops_count, const Config& c, Interference& interf) {
    (void)ctx;
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
    const double interf0 = interf.seconds;
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
        auto waited = scope.wait_for(oldest.ticket, std::chrono::milliseconds(20000));
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
        interf.touch(c.ws_bytes_per_op);
    }
    rusage after{};
    getrusage(RUSAGE_SELF, &after);
    out.wall = now_s() - t0;
    out.user = double(after.ru_utime.tv_sec - before.ru_utime.tv_sec) +
               double(after.ru_utime.tv_usec - before.ru_utime.tv_usec) * 1e-6;
    out.sys = double(after.ru_stime.tv_sec - before.ru_stime.tv_sec) +
              double(after.ru_stime.tv_usec - before.ru_stime.tv_usec) * 1e-6;
    out.interf = interf.seconds - interf0;
    return out;
}

struct ExternalShared {
    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t completions = 0;
    std::uint64_t notifications = 0;
    std::uint64_t empty_wakes = 0;
    std::uint64_t passes = 0;
    bool driver_attached = false;
    bool stop = false;
    bool driver_failed = false;
    std::string driver_failure;
    std::string shutdown_failure;
};

void external_driver(AsyncIoContext& ctx, int epoll_fd, int notification_fd,
                     ExternalShared& shared, int cpu, bool driver_interference,
                     std::size_t bytes_per_op, Interference& interf) {
    pin_self_to(cpu);
    auto owner = ctx.claim_progress_owner();
    if (!owner.has_value()) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.driver_failed = true;
        shared.driver_failure = "claim_progress_owner failed";
        shared.cv.notify_all();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.driver_attached = true;
        shared.cv.notify_all();
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            if (shared.stop)
                break;
        }
        epoll_event events[4];
        const int ready = ::epoll_wait(epoll_fd, events, 4, 1000);
        if (ready > 0) {
            for (int i = 0; i < ready; ++i) {
                if (events[i].data.fd == notification_fd) {
                    ctx.acknowledge_progress_notification();
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    ++shared.notifications;
                }
            }
        } else if (ready == 0) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            ++shared.empty_wakes;
        }
        std::size_t batch_completions = 0;
        bool failed = false;
        for (std::size_t guard = 0; guard < 1000000; ++guard) {
            auto pass = ctx.poll_progress();
            if (!pass.has_value()) {
                failed = true;
                break;
            }
            batch_completions += pass.value().completed;
            if (!pass.value().immediate_work_remains)
                break;
        }
        if (driver_interference && batch_completions > 0)
            interf.touch(bytes_per_op * batch_completions);
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            ++shared.passes;
            shared.completions += batch_completions;
            if (failed) {
                shared.driver_failed = true;
                shared.driver_failure = "poll_progress failed";
            }
        }
        shared.cv.notify_all();
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            if (shared.driver_failed)
                break;
        }
    }
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, notification_fd, nullptr) != 0) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.shutdown_failure = "EPOLL_CTL_DEL failed";
    }
    ctx.detach_progress_host();
    const auto outcome = ctx.shutdown(sluice::async::ShutdownPolicy::drain);
    if (!outcome.has_value()) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.shutdown_failure = "shutdown failed";
    }
}

struct ExtInflight {
    sluice::async::Request<std::size_t> request;
    std::uint64_t offset = 0;
    std::byte* buffer = nullptr;
};

PhaseResult run_external_timed(AsyncIoContext& ctx, ExternalShared& shared, const File& file,
                               std::size_t ops_count, const Config& c,
                               Interference* submitter_touch, Interference& meter) {
    PhaseResult out;
    std::vector<std::vector<std::byte>> buffers(c.depth, std::vector<std::byte>(c.payload));
    std::deque<ExtInflight> window;
    std::size_t submitted = 0;
    std::uint64_t watermark = 0;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        watermark = shared.completions;
    }
    rusage before{};
    getrusage(RUSAGE_SELF, &before);
    const double interf0 = meter.seconds;
    const double t0 = now_s();
    while (out.completed_ops < ops_count) {
        while (window.size() < c.depth && submitted < ops_count) {
            const std::size_t slot = submitted % c.depth;
            const std::size_t blocks =
                std::max<std::size_t>(c.payload * 64, 1u << 20) / c.payload;
            const std::uint64_t offset =
                static_cast<std::uint64_t>(submitted % blocks) * c.payload;
            sluice::async::ReadOp op;
            op.file = sluice::async::NativeFileRef{file};
            op.dst = buffers[slot].data();
            op.offset = offset;
            op.len = c.payload;
            auto result = ctx.submit_read(op);
            if (!result.has_value()) {
                out.ok = false;
                out.failure = "submit_read rejected";
                return out;
            }
            window.push_back(ExtInflight{std::move(result).value(), offset, buffers[slot].data()});
            ++submitted;
        }
        {
            std::unique_lock<std::mutex> lock(shared.mutex);
            const bool woke = shared.cv.wait_for(
                lock, std::chrono::seconds(30),
                [&] { return shared.completions > watermark || shared.driver_failed; });
            if (!woke) {
                out.ok = false;
                out.failure = "completion wait timed out";
                return out;
            }
            watermark = shared.completions;
            if (shared.driver_failed) {
                out.ok = false;
                out.failure = "driver failure: " + shared.driver_failure;
                return out;
            }
        }
        while (!window.empty() && window.front().request.ready()) {
            auto& front = window.front();
            const auto observed = front.request.take_result();
            if (observed.readiness != sluice::async::RequestReadiness::ready ||
                !observed.result.has_value() || observed.result.value() != c.payload ||
                !bytes_match(front.buffer, c.payload, front.offset)) {
                out.ok = false;
                out.failure = "read oracle mismatch";
                return out;
            }
            out.bytes_verified += c.payload;
            ++out.completed_ops;
            window.pop_front();
            if (submitter_touch)
                submitter_touch->touch(c.ws_bytes_per_op);
        }
    }
    rusage after{};
    getrusage(RUSAGE_SELF, &after);
    out.wall = now_s() - t0;
    out.user = double(after.ru_utime.tv_sec - before.ru_utime.tv_sec) +
               double(after.ru_utime.tv_usec - before.ru_utime.tv_usec) * 1e-6;
    out.sys = double(after.ru_stime.tv_sec - before.ru_stime.tv_sec) +
              double(after.ru_stime.tv_usec - before.ru_stime.tv_usec) * 1e-6;
    out.interf = meter.seconds - interf0;
    return out;
}

PhaseResult run_external(AsyncIoContext& ctx, const File& file, const Config& c,
                         Interference& submitter_interf, int driver_cpu) {
    PhaseResult out;
    const bool driver_side = (c.interference_side == "driver");
    Interference driver_interf(driver_side ? c.ws_bytes : 0);
    Interference* submitter_touch = driver_side ? nullptr : &submitter_interf;
    Interference& meter = driver_side ? driver_interf : submitter_interf;

    const int epoll_fd = ::epoll_create1(0);
    if (epoll_fd < 0) {
        out.ok = false;
        out.failure = "epoll_create1 failed";
        return out;
    }
    const int notification_fd = ctx.progress_notification_fd();
    if (notification_fd < 0) {
        out.ok = false;
        out.failure = "progress_notification_fd returned -1";
        ::close(epoll_fd);
        return out;
    }
    epoll_event registration{};
    registration.events = EPOLLIN;
    registration.data.fd = notification_fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, notification_fd, &registration) != 0) {
        out.ok = false;
        out.failure = "EPOLL_CTL_ADD failed";
        ::close(epoll_fd);
        return out;
    }

    ExternalShared shared;
    std::thread driver(external_driver, std::ref(ctx), epoll_fd, notification_fd,
                       std::ref(shared), driver_cpu, driver_side, c.ws_bytes_per_op,
                       std::ref(driver_interf));
    {
        std::unique_lock<std::mutex> lock(shared.mutex);
        shared.cv.wait_for(lock, std::chrono::seconds(10),
                           [&] { return shared.driver_attached || shared.driver_failed; });
        if (shared.driver_failed) {
            out.ok = false;
            out.failure = "driver attach failed: " + shared.driver_failure;
            driver.join();
            ::close(epoll_fd);
            return out;
        }
    }

    if (c.warmup > 0) {
        PhaseResult warm = run_external_timed(ctx, shared, file, c.warmup, c,
                                              submitter_touch, meter);
        if (!warm.ok) {
            out.ok = false;
            out.failure = "warmup: " + warm.failure;
        }
    }
    PhaseResult timed;
    if (out.ok)
        timed = run_external_timed(ctx, shared, file, c.ops, c, submitter_touch, meter);

    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.stop = true;
    }
    shared.cv.notify_all();
    ctx.request_stop(sluice::async::ShutdownPolicy::drain);
    driver.join();
    ::close(epoll_fd);

    if (out.ok)
        out = timed;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (out.ok && !shared.shutdown_failure.empty()) {
            out.ok = false;
            out.failure = shared.shutdown_failure;
        }
        if (out.ok && shared.driver_failed) {
            out.ok = false;
            out.failure = "driver: " + shared.driver_failure;
        }
        if (out.ok) {
            out.notifications = shared.notifications;
            out.empty_wakes = shared.empty_wakes;
            out.passes = shared.passes;
        }
    }
    if (driver_side) {
        submitter_interf.seconds = driver_interf.seconds;
        submitter_interf.sink = driver_interf.sink;
    }
    return out;
}

} // namespace

#if defined(SLUICE_URING_LOOKUP_INSTRUMENT)
namespace {
void emit_counters_line(const Config& c) {
    namespace r = sluice::async::research;
    const r::RouterCounters& rc = r::g_router_counters;
    std::printf("{\"kind\":\"counters\",\"label\":\"%s\",\"router\":{\"installs\":%llu,"
                "\"retires\":%llu,\"cq_operation\":%llu,\"cq_control\":%llu,"
                "\"capacity\":%llu},\"sites\":{",
                c.label.c_str(),
                (unsigned long long)rc.installs.load(), (unsigned long long)rc.retires.load(),
                (unsigned long long)rc.cq_operation.load(),
                (unsigned long long)rc.cq_control.load(),
                (unsigned long long)rc.router_capacity.load());
    const char* names[] = {"op_cqe", "control_cqe", "mark_consumed_control",
                           "cancel_by_handle", "poison_class_a"};
    bool first = true;
    for (unsigned s = 0; s < static_cast<unsigned>(r::LookupSite::kSiteCount); ++s) {
        const r::LookupCounters& lc = r::g_lookup_counters[s];
        const unsigned long long attempts = lc.attempts.load();
        if (attempts == 0 && s != 0)
            continue;
        std::uint64_t total = 0;
        for (std::size_t b = 1; b < 66; ++b)
            total += lc.probe_hist[b].load();
        auto pct = [&](double p) -> unsigned long long {
            if (total == 0)
                return 0;
            std::uint64_t target = static_cast<std::uint64_t>(p * double(total));
            std::uint64_t cum = 0;
            for (std::size_t b = 1; b < 66; ++b) {
                cum += lc.probe_hist[b].load();
                if (cum >= target)
                    return b;
            }
            return 65;
        };
        std::printf("%s\"%s\":{\"attempts\":%llu,\"hits\":%llu,\"misses\":%llu,"
                    "\"avg_probes\":%.2f,\"p50\":%llu,\"p99\":%llu,\"max\":%llu,"
                    "\"avg_live\":%.2f}",
                    first ? "" : ",", names[s], attempts,
                    (unsigned long long)lc.hits.load(), (unsigned long long)lc.misses.load(),
                    attempts ? double(lc.probe_sum.load()) / double(attempts) : 0.0,
                    pct(0.50), pct(0.99), (unsigned long long)lc.probe_max.load(),
                    attempts ? double(lc.live_at_attempt_sum.load()) / double(attempts) : 0.0);
        first = false;
    }
    std::printf("}}\n");
    std::fflush(stdout);
}
} // namespace
#endif

int main(int argc, char** argv) {
    Config c = parse_args(argc, argv);
    if (c.scenario != "pipeline" && c.scenario != "external") {
        std::fprintf(stderr, "scenario must be pipeline or external\n");
        return 2;
    }
    if (c.scenario == "pipeline" && c.pin_mode == "split") {
        std::fprintf(stderr, "pipeline is single-owner by construction; "
                             "split core requires the external scenario\n");
        return 2;
    }
    if (c.scenario == "pipeline" && c.interference_side == "driver") {
        std::fprintf(stderr, "pipeline has no driver thread; interference runs on the "
                             "owner thread\n");
        return 2;
    }
    if (c.pin_mode == "single" || c.pin_mode == "split")
        pin_self_to(c.cpu_a);
    const int driver_cpu = (c.pin_mode == "split") ? c.cpu_b : c.cpu_a;

    const std::uint64_t salt = static_cast<std::uint64_t>(::time(nullptr));
    auto fixture = make_fixture(c, salt);
    if (!fixture.has_value()) {
        std::fprintf(stderr, "fixture failed\n");
        return 1;
    }
    File file = std::move(fixture).value();

    const std::size_t capacity = c.capacity ? c.capacity : c.depth;
    sluice::async::UringConfig config;
    config.request_capacity = capacity;
    config.queue_depth = static_cast<unsigned>(std::max<std::size_t>(capacity, 8));
    auto made = sluice::async::make_uring_context(config, nullptr);
    if (!made.has_value()) {
        std::fprintf(stderr, "uring context failed\n");
        return 1;
    }
    AsyncIoContext ctx = std::move(made).value();

    PhaseResult out;
    Interference interf(c.ws_bytes);
    std::string warm_failure;
    if (c.scenario == "pipeline") {
        RequestScope scope(ctx, std::max(c.depth, std::size_t(1)),
                           sluice::async::ScopeCleanupPolicy::drain);
        if (c.warmup > 0) {
            PhaseResult warm = run_pipeline(ctx, scope, file, c.warmup, c, interf);
            if (!warm.ok)
                warm_failure = warm.failure;
        }
        if (warm_failure.empty())
            out = run_pipeline(ctx, scope, file, c.ops, c, interf);
        auto fin = scope.finish();
        if (!fin.has_value() && warm_failure.empty() && out.ok) {
            out.ok = false;
            out.failure = "scope finish failed";
        }
    } else {
        out = run_external(ctx, file, c, interf, driver_cpu);
    }

    const double net_wall = (c.scenario == "pipeline") ? out.wall - out.interf : -1.0;
    std::printf(
        "{\"kind\":\"timed\",\"label\":\"%s\",\"scenario\":\"%s\",\"pin\":\"%s\","
        "\"side\":\"%s\",\"capacity\":%zu,\"depth\":%zu,\"ops\":%zu,\"payload\":%zu,"
        "\"ws_bytes\":%zu,\"ws_per_op\":%zu,\"warmup\":%zu,\"completed_ops\":%zu,"
        "\"bytes_verified\":%zu,\"wall_s\":%.6f,\"user_s\":%.6f,\"sys_s\":%.6f,"
        "\"interf_s\":%.6f,\"net_wall_s\":%.6f,\"sink\":%llu,\"notifications\":%llu,"
        "\"passes\":%llu,\"ok\":%s,\"failure\":\"%s\",\"warmup_failure\":\"%s\"}\n",
        c.label.c_str(), c.scenario.c_str(), c.pin_mode.c_str(),
        c.interference_side.c_str(), capacity, c.depth, c.ops, c.payload, c.ws_bytes,
        c.ws_bytes_per_op, c.warmup, out.completed_ops, out.bytes_verified, out.wall,
        out.user, out.sys, out.interf, net_wall, (unsigned long long)interf.sink,
        (unsigned long long)out.notifications, (unsigned long long)out.passes,
        out.ok ? "true" : "false", out.failure.c_str(), warm_failure.c_str());
    std::fflush(stdout);

#if defined(SLUICE_URING_LOOKUP_INSTRUMENT)
    emit_counters_line(c);
#endif

    (void)unlink(fixture_path(c, salt).c_str());
    return (out.ok && warm_failure.empty()) ? 0 : 1;
}
