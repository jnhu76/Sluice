#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::FileAccess;
using sluice::IoError;

int g_failures = 0;

[[maybe_unused]] void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::fflush(stderr);
        ++g_failures;
    }
}


constexpr auto kDriverExitBudget = std::chrono::seconds(10);
constexpr auto kCompletionBudget = std::chrono::seconds(20);

// Model of the bench_w03_external_loop driver: the driver re-checks the
// shared stop flag only between epoll_wait returns, so the stop flag must be
// published before request_stop's unconditional wake. The park/wake counters
// are the seam that makes the losing interleaving observable; a driver past
// them can never see a stop-flag write that arrives with no further wake.
struct StopProtocol {
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    bool driver_attached = false;
    bool driver_exited = false;
    bool driver_failed = false;
    std::string driver_failure;
    std::uint64_t notifications = 0;
    std::uint64_t passes = 0;
    std::uint64_t completions = 0;
    std::atomic<std::uint32_t> park_generation{0};
    std::atomic<std::uint32_t> wakes_consumed{0};
};

void drive_external_loop(AsyncIoContext& ctx, int epoll_fd, int notification_fd,
                         StopProtocol& shared) {
    auto owner = ctx.claim_progress_owner();
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (owner.has_value()) {
            shared.driver_attached = true;
        } else {
            shared.driver_failed = true;
            shared.driver_failure = "claim_progress_owner failed";
        }
        shared.cv.notify_all();
    }
    if (!owner.has_value()) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.driver_exited = true;
        shared.cv.notify_all();
        return;
    }
    for (;;) {
        bool stopping = false;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            stopping = shared.stop;
        }
        if (stopping) {
            break;
        }
        shared.park_generation.fetch_add(1, std::memory_order_release);
        shared.park_generation.notify_all();
        epoll_event events[2];
        const int ready = ::epoll_wait(epoll_fd, events, 2, -1);
        if (ready > 0) {
            for (int i = 0; i < ready; ++i) {
                if (events[i].data.fd == notification_fd) {
                    ctx.acknowledge_progress_notification();
                    shared.wakes_consumed.fetch_add(1, std::memory_order_release);
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    ++shared.notifications;
                }
            }
        }
        std::size_t batch = 0;
        bool pass_failed = false;
        for (int guard = 0; guard < 100000; ++guard) {
            auto pass = ctx.poll_progress();
            if (!pass.has_value()) {
                pass_failed = true;
                break;
            }
            batch += pass.value().completed;
            if (!pass.value().immediate_work_remains) {
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            ++shared.passes;
            shared.completions += batch;
            if (pass_failed) {
                shared.driver_failed = true;
                shared.driver_failure = "poll_progress failed";
            }
        }
        shared.cv.notify_all();
    }
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, notification_fd, nullptr) != 0) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.driver_failed = true;
        shared.driver_failure = "EPOLL_CTL_DEL failed";
    }
    ctx.detach_progress_host();
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    if (!outcome.has_value()) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.driver_failed = true;
        shared.driver_failure = "shutdown failed";
    }
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.driver_exited = true;
        shared.cv.notify_all();
    }
}

// Bounded wait on the driver-exit flag: the cap only bounds the failure mode
// of a future regression, the pass condition is the flag itself.
bool wait_driver_exited(StopProtocol& shared, std::chrono::seconds budget) {
    std::unique_lock<std::mutex> lock(shared.mutex);
    return shared.cv.wait_for(lock, budget, [&] { return shared.driver_exited; });
}

struct LoopFixture {
    AsyncIoContext ctx;
    int epoll_fd = -1;
    int notification_fd = -1;
    StopProtocol shared;
    std::thread driver;

    explicit LoopFixture(AsyncIoContext&& context) : ctx(std::move(context)) {}
    ~LoopFixture() {
        if (epoll_fd >= 0) {
            ::close(epoll_fd);
        }
    }
    LoopFixture(const LoopFixture&) = delete;
    LoopFixture& operator=(const LoopFixture&) = delete;

    static std::unique_ptr<LoopFixture> start(std::size_t capacity) {
        ThreadPoolConfig config;
        config.request_capacity = capacity;
        config.worker_count = 2;
        std::unique_ptr<LoopFixture> fixture(new LoopFixture(make_threadpool_context(config)));
        fixture->epoll_fd = ::epoll_create1(0);
        if (fixture->epoll_fd < 0) {
            return nullptr;
        }
        fixture->notification_fd = fixture->ctx.progress_notification_fd();
        if (fixture->notification_fd < 0) {
            return nullptr;
        }
        epoll_event registration{};
        registration.events = EPOLLIN;
        registration.data.fd = fixture->notification_fd;
        if (::epoll_ctl(fixture->epoll_fd, EPOLL_CTL_ADD, fixture->notification_fd,
                        &registration) != 0) {
            return nullptr;
        }
        fixture->driver = std::thread(drive_external_loop, std::ref(fixture->ctx),
                                      fixture->epoll_fd, fixture->notification_fd,
                                      std::ref(fixture->shared));
        std::unique_lock<std::mutex> lock(fixture->shared.mutex);
        const bool attached = fixture->shared.cv.wait_for(
            lock, kCompletionBudget, [&] { return fixture->shared.driver_attached; });
        lock.unlock();
        if (!attached) {
            return nullptr;
        }
        return fixture;
    }
};

#if !defined(SLUICE_EXTERNAL_LOOP_MUTANT_STOP_FLAG_AFTER_WAKE)

// The fixed stop ordering under test. The flag write happens under the same
// mutex the driver reads it from, before request_stop; the wake therefore
// cannot be consumed before the condition it advertises is visible.
void stop_and_join_fixed_order(AsyncIoContext& ctx, StopProtocol& shared,
                               std::thread& driver) {
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.stop = true;
    }
    shared.cv.notify_all();
    ctx.request_stop(ShutdownPolicy::drain);
    if (!wait_driver_exited(shared, kDriverExitBudget)) {
        std::fprintf(stderr,
                     "FAIL: driver did not exit after the fixed-order stop sequence; "
                     "aborting instead of hanging on join\n");
        std::fflush(stderr);
        std::terminate();
    }
    driver.join();
    check(!shared.driver_failed, "driver completed without failure");
    if (shared.driver_failed) {
        std::fprintf(stderr, "  driver failure: %s\n", shared.driver_failure.c_str());
    }
}

// A fault that blocks the modeled teardown (a pending request or a running
// driver cannot be discarded by contract): report and leave before the
// destructor fail-fast can mask the reason.
[[noreturn]] void fail_hard(const char* what) {
    std::fprintf(stderr, "FAIL: %s; abandoning the process before unsafe teardown\n", what);
    std::fflush(stderr);
    ::_exit(1);
}

int temp_file_with(const char* pattern, const std::byte* data, std::size_t len) {
    char path[64];
    std::snprintf(path, sizeof(path), "%s", pattern);
    const int fd = ::mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    ::unlink(path);
    std::size_t written = 0;
    while (written < len) {
        const ssize_t n = ::write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return -1;
        }
        written += static_cast<std::size_t>(n);
    }
    return fd;
}

// The regression itself: with the stop flag published before the wake, a
// driver parked in epoll_wait consumes request_stop's wake, observes the
// flag and exits, so join returns. Under the STOP_FLAG_AFTER_WAKE mutant the
// same interleaving strands the driver (the mutant target reports the
// controlled failure).
void stop_flag_published_before_wake_joins() {
    auto fixture = LoopFixture::start(8);
    if (!fixture) {
        fail_hard("idle-loop fixture started");
    }
    // Force the interleaving the mutant strands: the driver must be parked in
    // epoll_wait before the stop sequence runs, so request_stop's wake is
    // guaranteed to reach it.
    const auto park_deadline = std::chrono::steady_clock::now() + kCompletionBudget;
    std::uint32_t parks = fixture->shared.park_generation.load(std::memory_order_acquire);
    while (parks < 1) {
        if (std::chrono::steady_clock::now() > park_deadline) {
            fail_hard("driver parked in epoll_wait before the stop sequence");
        }
        fixture->shared.park_generation.wait(parks, std::memory_order_acquire);
        parks = fixture->shared.park_generation.load(std::memory_order_acquire);
    }
    stop_and_join_fixed_order(fixture->ctx, fixture->shared, fixture->driver);
    check(fixture->shared.wakes_consumed.load() >= 1,
          "driver consumed the request_stop wake and still exited");
    check(fixture->shared.notifications >= 1, "wake counted through the acknowledgement path");
}

void completed_phase_stop_joins() {
    auto fixture = LoopFixture::start(8);
    if (!fixture) {
        fail_hard("workload fixture started");
    }
    constexpr std::size_t kOps = 8;
    constexpr std::size_t kLen = 4096;
    std::vector<std::byte> pattern(kLen);
    for (std::size_t i = 0; i < kLen; ++i) {
        pattern[i] = static_cast<std::byte>(i & 0xff);
    }
    const int fd = temp_file_with("/tmp/sluice_stoporder_XXXXXX", pattern.data(), kLen);
    if (fd < 0) {
        fail_hard("fixture file created");
    }

    std::vector<std::vector<std::byte>> bufs(kOps, std::vector<std::byte>(kLen));
    std::vector<Request<std::size_t>> requests;
    requests.reserve(kOps);
    for (std::size_t i = 0; i < kOps; ++i) {
        ReadOp op;
        op.file = NativeFileRef(fd, FileAccess::read_only);
        op.dst = bufs[i].data();
        op.len = kLen;
        op.offset = 0;
        auto submitted = fixture->ctx.submit_read(op);
        if (!submitted.has_value()) {
            fail_hard("read submitted");
        }
        requests.push_back(std::move(submitted).value());
    }

    bool settled = false;
    {
        std::unique_lock<std::mutex> lock(fixture->shared.mutex);
        settled = fixture->shared.cv.wait_for(
            lock, kCompletionBudget, [&] { return fixture->shared.completions >= kOps; });
    }
    if (!settled) {
        fail_hard("all driven completions observed through the external loop");
    }
    for (auto& request : requests) {
        if (request.ready()) {
            const auto observed = request.take_result();
            check(observed.result.has_value() && observed.result.value() == kLen,
                  "driven read completed with full length");
        } else {
            fail_hard("request ready after its completion was counted");
        }
    }
    ::close(fd);

    stop_and_join_fixed_order(fixture->ctx, fixture->shared, fixture->driver);
    check(fixture->shared.completions == kOps, "no completion lost across stop");
}

void early_failure_stop_joins() {
    auto fixture = LoopFixture::start(8);
    if (!fixture) {
        fail_hard("early-failure fixture started");
    }
    std::vector<std::byte> buf(64);
    ReadOp op;
    op.file = NativeFileRef(-1, FileAccess::read_only);
    op.dst = buf.data();
    op.len = buf.size();
    op.offset = 0;
    const auto rejected = fixture->ctx.submit_read(op);
    if (rejected.has_value()) {
        fail_hard("negative-fd read is deterministically rejected");
    }
    check(rejected.error().code == IoError::Code::invalid_state,
          "negative-fd read rejected as invalid state");

    stop_and_join_fixed_order(fixture->ctx, fixture->shared, fixture->driver);
}

int run_fixed_suite() {
    stop_flag_published_before_wake_joins();
    completed_phase_stop_joins();
    early_failure_stop_joins();
    if (g_failures == 0) {
        std::printf("external_loop_stop_order: all scenarios passed\n");
        return 0;
    }
    std::printf("external_loop_stop_order: failures=%d\n", g_failures);
    return 1;
}

#else

[[noreturn]] void run_mutant() {
    auto fixture = LoopFixture::start(8);
    if (!fixture) {
        std::fprintf(stderr, "MUTANT UNTESTABLE: fixture failed to start\n");
        std::fflush(stderr);
        ::_exit(3);
    }
    // Mutant ordering: request_stop's unconditional wake races the flag. Wait
    // until the driver has consumed that wake and re-parked with the flag
    // still false; from here no further wake exists and the driver can never
    // observe the flag.
    fixture->ctx.request_stop(ShutdownPolicy::drain);
    std::uint32_t parks = fixture->shared.park_generation.load(std::memory_order_acquire);
    while (parks < 2) {
        fixture->shared.park_generation.wait(parks, std::memory_order_acquire);
        parks = fixture->shared.park_generation.load(std::memory_order_acquire);
    }
    const std::uint32_t wakes = fixture->shared.wakes_consumed.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(fixture->shared.mutex);
        fixture->shared.stop = true;
    }
    fixture->shared.cv.notify_all();
    if (wait_driver_exited(fixture->shared, std::chrono::seconds(3))) {
        std::fprintf(stderr,
                     "MUTANT ESCAPED: driver exited although stop was published after "
                     "the consumed wake (park_generation=%u wakes_consumed=%u)\n",
                     parks, wakes);
        std::fflush(stderr);
        ::_exit(1);
    }
    std::fprintf(stderr,
                 "STOP_FLAG_AFTER_WAKE detected: driver consumed the request_stop wake "
                 "(park_generation=%u wakes_consumed=%u) and re-parked in epoll_wait(-1) "
                 "with stop=false; join() cannot return\n",
                 parks, wakes);
    std::fflush(stderr);
    ::_exit(2);
}

#endif

}  // namespace

#if defined(SLUICE_EXTERNAL_LOOP_MUTANT_STOP_FLAG_AFTER_WAKE)
int main() {
    run_mutant();
}
#else
int main() {
    return run_fixed_suite();
}
#endif
