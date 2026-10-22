// E1 W-03 external-loop A/B harness (research-only).
//
// Two modes. perf drives the shipped W-03 shape: a driver thread parks in a
// real epoll_wait on the context's real eventfd notification fd, acknowledges,
// and drives progress passes; a submitter thread runs a depth window over the
// tmpfs fixture. verify exercises the interleave obligations with bounded
// watchdogs: idle park/wake, zero-length operations, notification coalescing
// (eventfd counter K>0), a real kernel cancellation of a pending pipe read
// racing other completions, and detach/shutdown terminal responsibility. The
// same source is compiled against each arm's static library; the only per-arm
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
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <sys/epoll.h>
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
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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
using sluice::async::Request;
using sluice::async::RequestReadiness;
using sluice::async::ShutdownPolicy;
using sluice::async::UringConfig;

constexpr std::size_t kFixtureBytes = 64u << 20;

struct RusageSnapshotLike {
    double utime_s = 0.0;
    double stime_s = 0.0;
    long nvcsw = 0;
    long nivcsw = 0;
};

struct Params {
    std::string arm = "unlabeled";
    std::string mode = "perf";
    std::string scenario = "all";
    std::size_t block = 4096;
    std::size_t depth = 8;
    std::size_t capacity = 1024;
    std::size_t requests = 20000;
    std::size_t warmup = 1;
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

struct SharedLoop {
    std::mutex mutex;
    std::condition_variable cv;
    bool driver_attached = false;
    bool driver_failed = false;
    bool stop = false;
    std::string driver_failure;
    std::string shutdown_failure;
    std::uint64_t notifications = 0;
    std::uint64_t empty_wakes = 0;
    std::uint64_t passes = 0;
    std::uint64_t completions = 0;
};

struct LoopOutcome {
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
    std::uint64_t allocations = 0;
    std::uint64_t notifications = 0;
    std::uint64_t empty_wakes = 0;
    std::uint64_t passes = 0;
    AsyncStats stats;
};

void emit_row(const Params& p, const char* phase, const std::string& scenario,
              const LoopOutcome& o, bool no_lost_wake) {
    const double cpu_ns_per_op = o.ops > 0 ? (o.cpu_s * 1e9) / static_cast<double>(o.ops) : 0.0;
    std::printf(
        "{\"arm\":\"%s\",\"loop_kind\":\"epoll+eventfd\",\"mode\":\"%s\",\"scenario\":\"%s\","
        "\"block\":%zu,\"depth\":%zu,\"capacity\":%zu,\"requests\":%zu,"
        "\"phase\":\"%s\",\"ok\":%s,\"no_lost_wake\":%s,"
        "\"wall_s\":%.9f,\"cpu_ns_per_op\":%.3f,\"ops\":%zu,\"bytes\":%zu,"
        "\"utime_ns_per_op\":%.3f,\"stime_ns_per_op\":%.3f,"
        "\"nvcsw\":%ld,\"nivcsw\":%ld,\"allocations\":%llu,"
        "\"notifications\":%llu,\"empty_wakes\":%llu,\"passes\":%llu,"
        "\"submit_calls\":%llu,\"completed_ops\":%llu,\"canceled_ops\":%llu,"
        "\"completion_errors\":%llu,\"invalid_state\":%llu,\"failure\":\"%s\"}\n",
        p.arm.c_str(), p.mode.c_str(), scenario.c_str(), p.block, p.depth, p.capacity,
        p.requests, phase, o.ok ? "true" : "false", no_lost_wake ? "true" : "false",
        o.wall_s, cpu_ns_per_op, o.ops, o.bytes,
        o.ops > 0 ? (o.utime_s * 1e9) / static_cast<double>(o.ops) : 0.0,
        o.ops > 0 ? (o.stime_s * 1e9) / static_cast<double>(o.ops) : 0.0,
        o.nvcsw, o.nivcsw, static_cast<unsigned long long>(o.allocations),
        static_cast<unsigned long long>(o.notifications),
        static_cast<unsigned long long>(o.empty_wakes),
        static_cast<unsigned long long>(o.passes),
        static_cast<unsigned long long>(o.stats.submit_calls),
        static_cast<unsigned long long>(o.stats.completed_ops),
        static_cast<unsigned long long>(o.stats.canceled_ops),
        static_cast<unsigned long long>(o.stats.completion_errors),
        static_cast<unsigned long long>(o.stats.invalid_state_rejections),
        o.failure.c_str());
    std::fflush(stdout);
}

void driver_thread(AsyncIoContext& ctx, int epoll_fd, int notification_fd, SharedLoop& shared) {
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

    bool retry_pending = false;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            if (shared.stop)
                break;
        }
        epoll_event events[4];
        const int timeout_ms = retry_pending ? 1 : -1;
        const int ready = ::epoll_wait(epoll_fd, events, 4, timeout_ms);
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
        bool retry_remains = false;
        bool pass_failed = false;
        for (std::size_t guard = 0; guard < 1000000; ++guard) {
            auto pass = ctx.poll_progress();
            if (!pass.has_value()) {
                pass_failed = true;
                break;
            }
            batch_completions += pass.value().completed;
            retry_remains = pass.value().dispatch_retry_remains;
            if (!pass.value().immediate_work_remains)
                break;
        }
        retry_pending = retry_remains;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            ++shared.passes;
            shared.completions += batch_completions;
            if (pass_failed) {
                shared.driver_failed = true;
                shared.driver_failure = "poll_progress failed";
            }
        }
        shared.cv.notify_all();
    }

    if (::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, notification_fd, nullptr) != 0) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.shutdown_failure = "EPOLL_CTL_DEL failed";
    }
    ctx.detach_progress_host();
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    if (!outcome.has_value()) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.shutdown_failure = "shutdown failed";
    }
    shared.cv.notify_all();
}

struct DriverHandle {
    AsyncIoContext& ctx;
    int epoll_fd;
    int notification_fd;
    SharedLoop& shared;
    std::thread thread;

    ~DriverHandle() {
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.stop = true;
        }
        // request_stop wakes the parked progress owner so the driver thread
        // can observe the stop flag; without it epoll_wait(-1) never returns.
        ctx.request_stop(ShutdownPolicy::drain);
        shared.cv.notify_all();
        if (thread.joinable())
            thread.join();
        ::close(epoll_fd);
    }
};

DriverHandle start_driver(AsyncIoContext& ctx, SharedLoop& shared, std::string& failure) {
    const int epoll_fd = ::epoll_create1(0);
    if (epoll_fd < 0) {
        failure = "epoll_create1 failed";
        return DriverHandle{ctx, -1, -1, shared, std::thread{}};
    }
    const int notification_fd = ctx.progress_notification_fd();
    if (notification_fd < 0) {
        failure = "progress_notification_fd returned -1";
        ::close(epoll_fd);
        return DriverHandle{ctx, -1, -1, shared, std::thread{}};
    }
    epoll_event registration{};
    registration.events = EPOLLIN;
    registration.data.fd = notification_fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, notification_fd, &registration) != 0) {
        failure = "EPOLL_CTL_ADD failed";
        ::close(epoll_fd);
        return DriverHandle{ctx, -1, -1, shared, std::thread{}};
    }
    return DriverHandle{ctx, epoll_fd, notification_fd, shared,
                        std::thread(driver_thread, std::ref(ctx), epoll_fd, notification_fd,
                                    std::ref(shared))};
}

std::optional<AsyncIoContext> make_context(const Params& p, AsyncStats& stats,
                                           std::string& failure) {
    try {
        UringConfig config;
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

LoopOutcome perf_phase(const Params& p, const std::string& path, bool verify) {
    LoopOutcome out;
    auto opened = File::open(path);
    if (!opened.has_value()) {
        out.failure = "open failed: " + describe(opened.error());
        return out;
    }
    File file = std::move(opened).value();
    AsyncStats stats;
    std::string failure;
    auto context = make_context(p, stats, failure);
    if (!context.has_value()) {
        out.failure = "NOT_RUN_WITH_REASON: " + failure;
        return out;
    }
    AsyncIoContext ctx = std::move(*context);

    SharedLoop shared;
    std::string driver_failure;
    DriverHandle driver = start_driver(ctx, shared, driver_failure);
    if (!driver.thread.joinable()) {
        out.failure = "driver start failed: " + driver_failure;
        return out;
    }
    {
        std::unique_lock<std::mutex> lock(shared.mutex);
        const bool attached = shared.cv.wait_for(lock, std::chrono::seconds(10),
                                                 [&] { return shared.driver_attached; });
        if (!attached) {
            out.failure = "driver did not attach within 10s";
            return out;
        }
    }

    const std::size_t blocks = (kFixtureBytes - p.block) / p.block;
    std::vector<std::vector<std::byte>> buffers(p.depth, std::vector<std::byte>(p.block));
    for (std::size_t i = 0; i < p.depth; ++i)
        bench::fill_pattern(buffers[i].data(), p.block, 0);

    struct Inflight {
        Request<std::size_t> request;
        std::size_t slot = 0;
        std::uint64_t offset = 0;
    };

    const RusageSnapshotLike rusage_before = take_rusage();
    const double cpu_before = bench::process_cpu_seconds();
    const std::uint64_t alloc_before = g_allocations.load(std::memory_order_relaxed);
    const bench::TimePoint start = bench::now();
    std::deque<Inflight> window;
    std::size_t submitted = 0;
    std::size_t settled = 0;
    std::uint64_t watermark = 0;
    bool failed = false;

    while (settled < p.requests && !failed) {
        while (window.size() < p.depth && submitted < p.requests) {
            const std::size_t slot = submitted % p.depth;
            const std::uint64_t offset =
                static_cast<std::uint64_t>(submitted % blocks) * p.block;
            ReadOp op;
            op.file = NativeFileRef(file);
            op.dst = buffers[slot].data();
            op.len = p.block;
            op.offset = offset;
            auto result = ctx.submit_read(op);
            if (!result.has_value()) {
                out.failure = "submit_read rejected: " + describe(result.error());
                failed = true;
                break;
            }
            window.push_back(Inflight{std::move(result).value(), slot, offset});
            ++submitted;
        }
        if (failed)
            break;
        if (window.empty()) {
            out.failure = "empty window while work remained";
            failed = true;
            break;
        }
        {
            std::unique_lock<std::mutex> lock(shared.mutex);
            const bool woke = shared.cv.wait_for(lock, std::chrono::seconds(20), [&] {
                return shared.completions > watermark || shared.driver_failed;
            });
            if (!woke) {
                out.failure = "completion wait timed out (no publication observed)";
                failed = true;
                break;
            }
            watermark = shared.completions;
            if (shared.driver_failed) {
                out.failure = "driver failure: " + shared.driver_failure;
                failed = true;
                break;
            }
        }
        while (!window.empty() && window.front().request.ready()) {
            Inflight& front = window.front();
            const auto observed = front.request.take_result();
            if (observed.readiness != RequestReadiness::ready || !observed.result.has_value()) {
                out.failure = "settled result was not successful";
                failed = true;
                break;
            }
            if (observed.result.value() != p.block) {
                out.failure = "short read: " + std::to_string(observed.result.value());
                failed = true;
                break;
            }
            if (verify) {
                const auto mismatch = bench::verify_pattern(buffers[front.slot].data(), p.block,
                                                            front.offset);
                if (!mismatch.ok) {
                    out.failure = "byte mismatch at absolute " +
                                  std::to_string(mismatch.first_bad_position);
                    failed = true;
                    break;
                }
            }
            out.bytes += observed.result.value();
            ++out.ops;
            ++settled;
            window.pop_front();
        }
    }
    const bench::TimePoint end = bench::now();
    const double cpu_after = bench::process_cpu_seconds();
    const RusageSnapshotLike rusage_after = take_rusage();
    const std::uint64_t alloc_after = g_allocations.load(std::memory_order_relaxed);

    out.wall_s = bench::seconds_between(start, end);
    out.cpu_s = cpu_after - cpu_before;
    out.utime_s = rusage_after.utime_s - rusage_before.utime_s;
    out.stime_s = rusage_after.stime_s - rusage_before.stime_s;
    out.nvcsw = rusage_after.nvcsw - rusage_before.nvcsw;
    out.nivcsw = rusage_after.nivcsw - rusage_before.nivcsw;
    out.allocations = alloc_after - alloc_before;
    out.ok = !failed;

    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.stop = true;
    }
    ctx.request_stop(ShutdownPolicy::drain);
    shared.cv.notify_all();
    driver.thread.join();
    ::close(driver.epoll_fd);

    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        out.notifications = shared.notifications;
        out.empty_wakes = shared.empty_wakes;
        out.passes = shared.passes;
        if (!shared.shutdown_failure.empty() && out.ok) {
            out.ok = false;
            out.failure = shared.shutdown_failure;
        }
        if (!shared.driver_failure.empty() && out.ok) {
            out.ok = false;
            out.failure = "driver: " + shared.driver_failure;
        }
    }
    out.stats = stats;
    return out;
}

// --- verify scenarios -------------------------------------------------------

struct VerifyResult {
    bool ok = false;
    bool no_lost_wake = true;
    std::string failure;
    std::uint64_t notifications = 0;
    std::uint64_t passes = 0;
};

bool wait_shared(SharedLoop& shared, std::unique_lock<std::mutex>& lock,
                 const std::function<bool()>& done, const char*& failure) {
    const bool woke = shared.cv.wait_for(lock, std::chrono::seconds(10), done);
    if (!woke) {
        failure = "bounded wait (10s) expired";
        return false;
    }
    return true;
}

// Idle park: with no work, the driver must stay parked in epoll_wait (no
// busy passes); a single submitted op must wake it once and settle.
VerifyResult verify_idle_park_wake(AsyncIoContext& ctx, DriverHandle& driver,
                                   SharedLoop& shared, const std::string& path) {
    VerifyResult r;
    auto opened = File::open(path);
    if (!opened.has_value()) {
        r.failure = "open failed: " + describe(opened.error());
        return r;
    }
    File file = std::move(opened).value();
    std::vector<std::byte> buffer(4096);
    bench::fill_pattern(buffer.data(), buffer.size(), 0);

    std::uint64_t passes_before = 0;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        passes_before = shared.passes;
    }
    // A real park needs the driver idle in epoll_wait; a short dwell with no
    // work proves it does not spin: pass count must stay flat.
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    std::uint64_t passes_idle = 0;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        passes_idle = shared.passes;
    }
    if (passes_idle > passes_before + 2) {
        r.failure = "driver spun while idle (pass count grew without work)";
        return r;
    }

    ReadOp op;
    op.file = NativeFileRef(file);
    op.dst = buffer.data();
    op.len = buffer.size();
    op.offset = 0;
    auto request = ctx.submit_read(op);
    if (!request.has_value()) {
        r.failure = "submit_read rejected: " + describe(request.error());
        return r;
    }
    const char* failure = nullptr;
    {
        std::unique_lock<std::mutex> lock(shared.mutex);
        if (!wait_shared(shared, lock, [&] { return request.value().ready(); }, failure)) {
            r.failure = failure;
            r.no_lost_wake = false;
            return r;
        }
        r.notifications = shared.notifications;
        r.passes = shared.passes;
    }
    const auto observed = request.value().take_result();
    r.ok = observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
           observed.result.value() == buffer.size();
    if (!r.ok)
        r.failure = "single op did not settle through the wake";
    return r;
}

// Zero-length operation through the real external loop.
VerifyResult verify_zero_length(AsyncIoContext& ctx, DriverHandle& driver,
                                SharedLoop& shared, const std::string& path) {
    VerifyResult r;
    auto opened = File::open(path);
    if (!opened.has_value()) {
        r.failure = "open failed: " + describe(opened.error());
        return r;
    }
    File file = std::move(opened).value();
    ReadOp op;
    op.file = NativeFileRef(file);
    op.dst = nullptr;
    op.len = 0;
    op.offset = 0;
    auto request = ctx.submit_read(op);
    if (!request.has_value()) {
        r.failure = "zero-length submit rejected: " + describe(request.error());
        return r;
    }
    const char* failure = nullptr;
    {
        std::unique_lock<std::mutex> lock(shared.mutex);
        if (!wait_shared(shared, lock, [&] { return request.value().ready(); }, failure)) {
            r.failure = failure;
            r.no_lost_wake = false;
            return r;
        }
    }
    const auto observed = request.value().take_result();
    r.ok = observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
           observed.result.value() == 0;
    if (!r.ok)
        r.failure = "zero-length op did not settle with a zero result";
    return r;
}

// Notification coalescing: a burst of completions against one parked driver
// must produce far fewer notifications than completions (eventfd counter K>0)
// while every completion is still delivered (no lost wake).
VerifyResult verify_burst_coalescing(AsyncIoContext& ctx, DriverHandle& driver,
                                     SharedLoop& shared, const std::string& path) {
    VerifyResult r;
    auto opened = File::open(path);
    if (!opened.has_value()) {
        r.failure = "open failed: " + describe(opened.error());
        return r;
    }
    File file = std::move(opened).value();
    constexpr std::size_t kBurst = 64;
    std::vector<std::vector<std::byte>> buffers(kBurst, std::vector<std::byte>(4096));
    std::vector<Request<std::size_t>> requests;
    for (std::size_t i = 0; i < kBurst; ++i) {
        ReadOp op;
        op.file = NativeFileRef(file);
        op.dst = buffers[i].data();
        op.len = buffers[i].size();
        op.offset = i * 4096;
        auto made = ctx.submit_read(op);
        if (!made.has_value()) {
            r.failure = "burst submit rejected: " + describe(made.error());
            return r;
        }
        requests.push_back(std::move(made).value());
    }
    const char* failure = nullptr;
    {
        std::unique_lock<std::mutex> lock(shared.mutex);
        if (!wait_shared(shared, lock,
                         [&] {
                             return std::all_of(requests.begin(), requests.end(),
                                                [](const Request<std::size_t>& q) {
                                                    return q.ready();
                                                });
                         },
                         failure)) {
            r.failure = failure;
            r.no_lost_wake = false;
            return r;
        }
        r.notifications = shared.notifications;
        r.passes = shared.passes;
    }
    bool all_ok = true;
    for (auto& q : requests) {
        const auto observed = q.take_result();
        all_ok = all_ok && observed.readiness == RequestReadiness::ready &&
                 observed.result.has_value() && observed.result.value() == 4096;
    }
    r.ok = all_ok;
    if (!all_ok)
        r.failure = "a burst op did not settle successfully";
    return r;
}

// Real kernel cancellation through the external loop: a pending pipe read is
// canceled while the driver loops; the sticky control path must converge and
// no completion may be lost.
VerifyResult verify_cancel_pending_pipe(AsyncIoContext& ctx, DriverHandle& driver,
                                        SharedLoop& shared) {
    VerifyResult r;
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        r.failure = "pipe creation failed";
        return r;
    }
    std::vector<std::byte> buffer(4096);
    ReadOp op;
    op.file = NativeFileRef(pipe_fds[0], sluice::FileAccess::read_only);
    op.dst = buffer.data();
    op.len = buffer.size();
    op.offset = 0;
    auto request = ctx.submit_read(op);
    if (!request.has_value()) {
        r.failure = "pipe read submit rejected: " + describe(request.error());
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return r;
    }
    const auto disposition = ctx.cancel(request.value().id());
    if (!disposition.has_value()) {
        r.failure = "cancel rejected: " + describe(disposition.error());
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return r;
    }
    const char* failure = nullptr;
    {
        std::unique_lock<std::mutex> lock(shared.mutex);
        if (!wait_shared(shared, lock, [&] { return request.value().ready(); }, failure)) {
            r.failure = failure;
            r.no_lost_wake = false;
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            return r;
        }
        r.notifications = shared.notifications;
        r.passes = shared.passes;
    }
    const auto observed = request.value().take_result();
    r.ok = observed.readiness == RequestReadiness::ready;
    if (r.ok && observed.result.has_value()) {
        // The read may win the race with the cancel; both outcomes are legal.
    } else if (!r.ok) {
        r.failure = "canceled pipe read did not reach a terminal state";
    }
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return r;
}

// Detach and shutdown with settled work: terminal responsibility must hold
// and shutdown must converge.
VerifyResult verify_detach_shutdown(AsyncIoContext& ctx, DriverHandle& driver,
                                    SharedLoop& shared, const std::string& path) {
    VerifyResult r;
    auto opened = File::open(path);
    if (!opened.has_value()) {
        r.failure = "open failed: " + describe(opened.error());
        return r;
    }
    File file = std::move(opened).value();
    std::vector<std::byte> buffer(4096);
    ReadOp op;
    op.file = NativeFileRef(file);
    op.dst = buffer.data();
    op.len = buffer.size();
    op.offset = 0;
    auto request = ctx.submit_read(op);
    if (!request.has_value()) {
        r.failure = "submit rejected: " + describe(request.error());
        return r;
    }
    const char* failure = nullptr;
    {
        std::unique_lock<std::mutex> lock(shared.mutex);
        if (!wait_shared(shared, lock, [&] { return request.value().ready(); }, failure)) {
            r.failure = failure;
            r.no_lost_wake = false;
            return r;
        }
    }
    const auto observed = request.value().take_result();
    r.ok = observed.readiness == RequestReadiness::ready && observed.result.has_value();
    if (!r.ok)
        r.failure = "work did not stay terminal across detach/shutdown";
    return r;
}

} // namespace

int main(int argc, char** argv) {
    ::alarm(600);
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
        else if (auto v = value("mode")) p.mode = *v;
        else if (auto v = value("scenario")) p.scenario = *v;
        else if (auto v = value("block")) p.block = std::strtoull(v->c_str(), nullptr, 10);
        else if (auto v = value("depth")) p.depth = std::strtoull(v->c_str(), nullptr, 10);
        else if (auto v = value("capacity")) p.capacity = std::strtoull(v->c_str(), nullptr, 10);
        else if (auto v = value("requests")) p.requests = std::strtoull(v->c_str(), nullptr, 10);
        else if (auto v = value("warmup")) p.warmup = std::strtoull(v->c_str(), nullptr, 10);
        else if (auto v = value("fixture")) fixture = *v;
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

    const bench::OracleSelfTest oracle_gate = bench::oracle_self_test();
    if (!oracle_gate.ok) {
        std::fprintf(stderr, "oracle self-test failed: %s\n", oracle_gate.failure.c_str());
        return 1;
    }

    if (p.mode == "verify") {
        // Each verify scenario runs its own context + driver so the
        // detach/shutdown scenario ends on a fresh convergence path.
        struct Scenario {
            const char* name;
        };
        const Scenario scenarios[] = {{"idle_park_wake"}, {"zero_length"},
                                      {"burst_coalescing"}, {"cancel_pending_pipe"},
                                      {"detach_shutdown"}};
        bool all_ok = true;
        for (const Scenario& s : scenarios) {
            if (p.scenario != std::string("all") && p.scenario != s.name)
                continue;
            AsyncStats stats;
            std::string failure;
            auto context = make_context(p, stats, failure);
            if (!context.has_value()) {
                emit_row(p, "verify", s.name, [&] {
                    LoopOutcome o;
                    o.ok = false;
                    o.failure = failure;
                    return o;
                }(), false);
                all_ok = false;
                continue;
            }
            AsyncIoContext ctx = std::move(*context);
            SharedLoop shared;
            std::string driver_failure;
            DriverHandle driver = start_driver(ctx, shared, driver_failure);
            if (!driver.thread.joinable()) {
                LoopOutcome o;
                o.ok = false;
                o.failure = "driver start failed: " + driver_failure;
                emit_row(p, "verify", s.name, o, false);
                all_ok = false;
                continue;
            }
            {
                std::unique_lock<std::mutex> lock(shared.mutex);
                shared.cv.wait_for(lock, std::chrono::seconds(10),
                                   [&] { return shared.driver_attached; });
            }
            VerifyResult r;
            if (s.name == std::string("idle_park_wake"))
                r = verify_idle_park_wake(ctx, driver, shared, fixture);
            else if (s.name == std::string("zero_length"))
                r = verify_zero_length(ctx, driver, shared, fixture);
            else if (s.name == std::string("burst_coalescing"))
                r = verify_burst_coalescing(ctx, driver, shared, fixture);
            else if (s.name == std::string("cancel_pending_pipe"))
                r = verify_cancel_pending_pipe(ctx, driver, shared);
            else if (s.name == std::string("detach_shutdown"))
                r = verify_detach_shutdown(ctx, driver, shared, fixture);
            LoopOutcome o;
            o.ok = r.ok;
            o.failure = r.failure;
            o.notifications = r.notifications;
            o.passes = r.passes;
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                o.notifications = shared.notifications;
                o.passes = shared.passes;
                if (!shared.driver_failure.empty() && o.ok) {
                    o.ok = false;
                    o.failure = "driver: " + shared.driver_failure;
                }
            }
            emit_row(p, "verify", s.name, o, r.no_lost_wake);
            all_ok = all_ok && r.ok;
        }
        return all_ok ? 0 : 1;
    }

    if (p.mode != "perf") {
        std::fprintf(stderr, "unknown mode: %s\n", p.mode.c_str());
        return 2;
    }

    for (std::size_t i = 0; i < p.warmup; ++i) {
        const LoopOutcome warm = perf_phase(p, fixture, false);
        if (!warm.ok) {
            emit_row(p, "warmup", "perf", warm, true);
            std::fprintf(stderr, "warmup failed: %s\n", warm.failure.c_str());
            return 1;
        }
    }
    const LoopOutcome timed = perf_phase(p, fixture, false);
    emit_row(p, "timed", "perf", timed, true);
    if (!timed.ok) {
        std::fprintf(stderr, "timed phase failed: %s\n", timed.failure.c_str());
        return 1;
    }
    const LoopOutcome oracle = perf_phase(p, fixture, true);
    emit_row(p, "oracle", "perf", oracle, true);
    if (!oracle.ok) {
        std::fprintf(stderr, "oracle phase failed: %s\n", oracle.failure.c_str());
        return 1;
    }
    return 0;
}
