// Clean-room micro-contract probe: the public wait/notify progress path,
// exercised end to end. Strong race discrimination stays with the internal
// deterministic tests. Built against the installed prefix.
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <mutex>
#include <thread>
#include <utility>
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

std::string make_temp_path() {
    char path[] = "/tmp/sluice_f1_cprog_XXXXXX";
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

int main() {
    using namespace sluice;
    using namespace sluice::async;

    const std::string path = make_temp_path();
    auto opened = File::open(path, create_read_write());
    CHECK(opened.has_value());
    File file = std::move(opened).value();
    const std::string payload = "progress-wake";
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    CHECK(sluice::blocking::write_all(file, bytes).has_value());

    AsyncIoContext ctx(
        std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 8,
                                                              .worker_count = 2}));

    const int epoll_fd = ::epoll_create1(0);
    CHECK(epoll_fd >= 0);
    const int notification_fd = ctx.progress_notification_fd();
    CHECK(notification_fd >= 0);
    epoll_event registered{};
    registered.events = EPOLLIN;
    registered.data.fd = notification_fd;
    CHECK(::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, notification_fd, &registered) == 0);

    epoll_event events[2];

    // Requests submitted off the owner thread are handed to the owner for
    // consumption; a nonterminal Request must never be destroyed.
    std::mutex handoff_mtx;
    std::vector<Request<std::size_t>> handoff;

    {
    auto owner = ctx.claim_progress_owner();
    CHECK(owner.has_value());

    // Drain to quiescence first: an empty bounded pass, so the subsequent
    // wake is attributable to the new submission.
    {
        auto pass = ctx.poll_progress();
        CHECK(pass.has_value());
        CHECK(!pass.value().immediate_work_remains);
    }

    std::vector<std::byte> buf(4, std::byte{0});
    bool woke_from_sleep = false;
    std::thread submitter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto r = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                        .dst = buf.data(),
                                        .len = buf.size(),
                                        .offset = 0});
        if (!r.has_value()) {
            std::fprintf(stderr, "FAIL: progress submit rejected\n");
            return;
        }
        const std::lock_guard<std::mutex> lock(handoff_mtx);
        handoff.push_back(std::move(r).value());
    });

    const int n = ::epoll_wait(epoll_fd, events, 2, 5000);
    CHECK(n > 0);
    for (int i = 0; i < n; ++i)
        if (events[i].data.fd == notification_fd) {
            ctx.acknowledge_progress_notification();
            woke_from_sleep = true;
        }
    CHECK(woke_from_sleep);
    submitter.join();

    // Bounded passes converge; wait_one observes the reaped completion or a
    // quiescent progress outcome without spinning.
    std::size_t completed = 0;
    for (int i = 0; i < 100; ++i) {
        auto pass = ctx.poll_progress();
        CHECK(pass.has_value());
        completed += pass.value().completed;
        if (!pass.value().immediate_work_remains && !pass.value().dispatch_retry_remains &&
            !pass.value().accepted_work_remains)
            break;
    }
    CHECK(completed >= 1);
    auto woke = ctx.wait_one(std::chrono::milliseconds(1));
    CHECK(woke.has_value());
    CHECK(woke.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress);
    CHECK(std::string(reinterpret_cast<const char*>(buf.data()), 4) ==
          payload.substr(0, 4));
    {
        const std::lock_guard<std::mutex> lock(handoff_mtx);
        for (auto& r : handoff) {
            while (!r.ready())
                (void)ctx.poll();
            (void)r.take_result();
        }
        handoff.clear();
    }

    // The retired notification interest can
    // be followed by a new borrow, and the new borrow wakes through the same
    // host loop for new work (descriptor reuse must not route stale events —
    // the old registration was deleted here via this new registration cycle;
    // the strong stale-wake discriminator is the internal shutdown oracle).
    CHECK(::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, notification_fd, nullptr) == 0);
    ctx.detach_progress_host();
    } // first owner handle destroyed: the claim detaches; a later driver may
      // attach again

    {
    auto owner2 = ctx.claim_progress_owner();
    CHECK(owner2.has_value());
    const int fd2 = ctx.progress_notification_fd();
    CHECK(fd2 >= 0);
    epoll_event re_registered{};
    re_registered.events = EPOLLIN;
    re_registered.data.fd = fd2;
    CHECK(::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd2, &re_registered) == 0);

    std::vector<std::byte> buf2(4, std::byte{0});
    std::thread submitter2([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto r = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                        .dst = buf2.data(),
                                        .len = buf2.size(),
                                        .offset = 4});
        if (!r.has_value()) {
            std::fprintf(stderr, "FAIL: second-cycle submit rejected\n");
            return;
        }
        const std::lock_guard<std::mutex> lock(handoff_mtx);
        handoff.push_back(std::move(r).value());
    });
    const int n2 = ::epoll_wait(epoll_fd, events, 2, 5000);
    CHECK(n2 > 0);
    bool second_wake = false;
    for (int i = 0; i < n2; ++i)
        if (events[i].data.fd == fd2) {
            ctx.acknowledge_progress_notification();
            second_wake = true;
        }
    CHECK(second_wake);
    submitter2.join();
    {
        const std::lock_guard<std::mutex> lock(handoff_mtx);
        for (auto& r : handoff) {
            while (!r.ready())
                (void)ctx.poll();
            (void)r.take_result();
        }
        handoff.clear();
    }

    CHECK(::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd2, nullptr) == 0);
    ctx.detach_progress_host();
    } // second owner handle destroyed
    CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
    ::close(epoll_fd);
    file.close();
    ::unlink(path.c_str());

    if (g_failures != 0) {
        std::fprintf(stderr, "contract_progress: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("contract_progress: PASS\n");
    return 0;
}
