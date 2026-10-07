// Clean-room consumer (W-03): application-owned external event loop. The
// application owns the loop and the progress owner, integrates the context's
// pollable notification fd through epoll, submits and reaps bounded work,
// handles interruption, and shuts down without Scheduler/Fiber or busy polling.
// Usage: w03_external_loop <threadpool|uring>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

constexpr int EXIT_UNAVAILABLE = 2;

std::string make_temp_path() {
    char path[] = "/tmp/sluice_f1_w03_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    return path;
}

sluice::FileOpen create_read_write() {
    sluice::FileOpen mode;
    mode.access = sluice::FileAccess::read_write;
    mode.existence = sluice::FileExistence::create_if_missing;
    return mode;
}

} // namespace

int main(int argc, char** argv) {
    using namespace sluice;
    using namespace sluice::async;

    if (argc < 2) {
        std::fprintf(stderr, "usage: w03_external_loop <threadpool|uring>\n");
        return 64;
    }
    const bool want_uring = std::strcmp(argv[1], "uring") == 0;

    std::unique_ptr<AsyncBackend> backend;
    if (want_uring) {
#if defined(SLUICE_HAS_LIBURING)
        try {
            backend = std::make_unique<UringAsyncBackend>(64u);
        } catch (const std::exception& error) {
            // Explicit unavailable outcome: constructing the named backend
            // fails loudly; nothing silently falls back to ThreadPool.
            std::printf("w03_uring: UNAVAILABLE (%s)\n", error.what());
            return EXIT_UNAVAILABLE;
        }
#else
        // Without the liburing profile the installed shell is abstract: the
        // named backend is not constructible from any external TU.
        std::printf("w03_uring: UNAVAILABLE (io_uring profile not built into this package)\n");
        return EXIT_UNAVAILABLE;
#endif
    } else {
        backend = std::make_unique<ThreadPoolBackend>(
            ThreadPoolConfig{.request_capacity = 8, .worker_count = 2});
    }

    const std::string path = make_temp_path();
    auto opened = File::open(path, create_read_write());
    CHECK(opened.has_value());
    File file = std::move(opened).value();
    const std::string payload = "external-loop-payload";
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    CHECK(sluice::blocking::write_all(file, bytes).has_value());

    AsyncIoContext ctx(std::move(backend));

    // The host registers the borrowed notification fd with its own epoll loop.
    const int epoll_fd = ::epoll_create1(0);
    CHECK(epoll_fd >= 0);
    const int notification_fd = ctx.progress_notification_fd();
    CHECK(notification_fd >= 0);
    epoll_event registered{};
    registered.events = EPOLLIN;
    registered.data.fd = notification_fd;
    CHECK(::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, notification_fd, &registered) == 0);

    // The application claims the progress owner and drives bounded passes;
    // the claim lives for the whole drive/shutdown cycle.
    {
        auto owner = ctx.claim_progress_owner();
        CHECK(owner.has_value());

    // Submit bounded work, reap through the external loop. A second thread
    // submits while the owner is parked in epoll_wait: the submission wakes
    // the sleeping owner without waiting for a completion.
    constexpr std::size_t kOps = 4;
    std::vector<std::vector<std::byte>> buffers(kOps, std::vector<std::byte>(8, std::byte{0}));
    std::vector<Request<std::size_t>> requests;
    requests.reserve(kOps);
    std::size_t reaped = 0;

    std::thread writer([&] {
        for (std::size_t i = 0; i < kOps; ++i) {
            auto r = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                            .dst = buffers[i].data(),
                                            .len = buffers[i].size(),
                                            .offset = i * 4});
            if (!r.has_value())
                std::fprintf(stderr, "FAIL: external submit %zu rejected\n", i);
            else
                requests.push_back(std::move(r).value());
        }
    });

    int wakeups = 0;
    while (reaped < kOps && wakeups < 1000) {
        epoll_event events[4];
        const int n = ::epoll_wait(epoll_fd, events, 4, 2000);
        ++wakeups;
        if (n <= 0)
            continue;
        for (int i = 0; i < n; ++i)
            if (events[i].data.fd == notification_fd)
                ctx.acknowledge_progress_notification();
        // Bounded nonblocking passes until the advertised work is done.
        for (;;) {
            auto pass = ctx.poll_progress();
            if (!pass.has_value()) {
                CHECK(false);
                break;
            }
            reaped += pass.value().completed;
            if (!pass.value().immediate_work_remains && !pass.value().dispatch_retry_remains)
                break;
        }
    }
    writer.join();
    CHECK(reaped == kOps);
    // All requests published; consume through the retained handles before they
    // go out of scope.
    for (auto& r : requests) {
        CHECK(r.ready());
        const auto observation = r.take_result();
        CHECK(observation.readiness == RequestReadiness::ready && observation.result.has_value());
    }
    requests.clear();
    for (std::size_t i = 0; i < kOps; ++i) {
        const std::string got(reinterpret_cast<const char*>(buffers[i].data()), 4);
        const std::string want = payload.substr(i * 4, 4);
        CHECK(got == want);
    }

    // Interruption: a non-owner thread requests stop; the owner observes the
    // control interrupt and applies the shutdown policy.
    std::thread stopper([&] { ctx.request_stop(ShutdownPolicy::drain); });
    stopper.join();
    CHECK(!ctx.admission_open());

    // The external registration is retired BEFORE driving shutdown: shutdown
    // refuses while a host notification interest is live.
    CHECK(::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, notification_fd, nullptr) == 0);
    ctx.detach_progress_host();
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    CHECK(outcome.has_value() && outcome.value() == ShutdownOutcome::completed);
    CHECK(ctx.execution_closed());
    } // owner handle destroyed here: the driving authority detaches

    ::close(epoll_fd);
    file.close();
    ::unlink(path.c_str());

    if (g_failures != 0) {
        std::fprintf(stderr, "w03_external_loop(%s): %d failure(s)\n", argv[1], g_failures);
        return 1;
    }
    std::printf("w03_external_loop(%s): PASS\n", argv[1]);
    return 0;
}
