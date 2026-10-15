// Diagnostic mirror of threadpool_external_loop_test's
// host_acknowledgement_racing_new_signal_never_strands with timestamps, to show
// where a 2000 ms poll timeout comes from. Diagnosis only; not repo evidence.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

using namespace sluice::async;
using sluice::FileAccess;

namespace {

auto t_start = std::chrono::steady_clock::now();

double ms() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - t_start)
               .count() /
           1000.0;
}

bool fd_readable(int fd) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    const int rc = ::poll(&p, 1, 0);
    return rc > 0 && (p.revents & POLLIN) != 0;
}

int temp_file_fd() {
    char path[] = "/tmp/sluice_race_probe_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd >= 0)
        ::unlink(path);
    return fd;
}

struct SharedRequest {
    std::optional<Request<std::size_t>> request;
    std::mutex mtx;

    bool submit_zero_op(AsyncIoContext& ctx) {
        std::vector<std::byte> buffer(4, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), FileAccess::read_only),
                                                 buffer.data(), 0, 0});
        if (!submitted.has_value())
            return false;
        std::lock_guard<std::mutex> lk(mtx);
        request = std::move(submitted.value());
        return true;
    }

    bool ready() {
        std::lock_guard<std::mutex> lk(mtx);
        return request.has_value() && request->ready();
    }
};

Request<std::size_t> submit_zero_op(AsyncIoContext& ctx) {
    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(temp_file_fd(), FileAccess::read_only),
                                             buffer.data(), 0, 0});
    if (!submitted.has_value())
        return {};
    return std::move(submitted).value();
}

std::size_t drive(AsyncIoContext& ctx, int& passes) {
    ctx.acknowledge_progress_notification();
    std::size_t delivered = 0;
    for (int i = 0; i < 64; ++i) {
        const auto pass = ctx.poll_progress();
        if (!pass.has_value())
            break;
        ++passes;
        delivered += pass.value().completed;
        if (pass.value().health_failed)
            break;
        if (pass.value().completed == 0 && !pass.value().immediate_work_remains)
            break;
    }
    return delivered;
}

} // namespace

int main() {
    std::atomic<bool> stop{false};
    std::thread watcher;
    auto start_watcher = [&](int nfd) {
        watcher = std::thread([&, nfd] {
            bool last = false;
            int ticks = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const bool r = fd_readable(nfd);
                if (++ticks % 2500 == 0) {
                    std::printf("phase=heartbeat t=%.3f readable=%d\n", ms(), int(r));
                    std::fflush(stdout);
                }
                if (r != last) {
                    std::printf("phase=readiness t=%.3f readable=%d\n", ms(), int(r));
                    std::fflush(stdout);
                    last = r;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    };
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 1});
    AsyncIoContext ctx(std::move(backend));
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return 2;
    std::optional<ProgressOwner> owner = std::move(claimed).value();
    const int nfd = ctx.progress_notification_fd();
    start_watcher(nfd);

    Request<std::size_t> warm = submit_zero_op(ctx);
    if (!warm.valid())
        return 3;
    {
        bool readable = false;
        for (int i = 0; i < 5000 && !readable; i += 5)
            readable = fd_readable(nfd) || (std::this_thread::sleep_for(std::chrono::milliseconds{5}), false);
        std::printf("phase=warm t=%.3f readable=%d ready=%d\n", ms(), int(readable), int(warm.ready()));
    }
    int passes = 0;
    const std::size_t delivered = drive(ctx, passes);
    std::printf("phase=warm-drain t=%.3f delivered=%zu passes=%d fd_readable=%d\n", ms(), delivered,
                passes, int(fd_readable(nfd)));
    warm.discard();
    if (fd_readable(nfd))
        std::printf("phase=warm-quiescent t=%.3f fd still readable (unexpected)\n", ms());

    std::atomic<bool> acked{false};
    SharedRequest racing;
    std::thread signaler([&] {
        while (!acked.load(std::memory_order_acquire))
            std::this_thread::yield();
        const double entered = ms();
        const bool ok = racing.submit_zero_op(ctx);
        std::printf("phase=signaler t=%.3f entered=%.3f submitted=%d fd_readable=%d ready=%d\n", ms(),
                    entered, int(ok), int(fd_readable(nfd)), int(racing.ready()));
    });

    const bool readable_before_ack = fd_readable(nfd);
    const double ack_at = ms();
    acked.store(true, std::memory_order_release);
    ctx.acknowledge_progress_notification();
    std::printf("phase=ack t=%.3f before=%d after=%d\n", ack_at, int(readable_before_ack),
                int(fd_readable(nfd)));

    int wakes = 0;
    bool settled = false;
    while (wakes < 4) {
        const bool readable = fd_readable(nfd);
        std::printf("phase=loop t=%.3f wakes=%d fd_readable=%d ready=%d\n", ms(), wakes, int(readable),
                    int(racing.ready()));
        if (!readable) {
            if (racing.ready()) {
                settled = true;
                std::printf("phase=settled-by-readiness t=%.3f\n", ms());
                break;
            }
            struct pollfd p;
            p.fd = nfd;
            p.events = POLLIN;
            p.revents = 0;
            const double pre = ms();
            std::printf("phase=pre-poll t=%.3f\n", pre);
            std::fflush(stdout);
            const int rc = ::poll(&p, 1, 2000);
            std::printf("phase=post-poll t=%.3f waited=%.3f\n", ms(), ms() - pre);
            std::fflush(stdout);
            std::printf("phase=poll t=%.3f rc=%d errno=%d revents=%d\n", ms(), rc, rc < 0 ? errno : 0,
                        p.revents);
            if (rc <= 0)
                break;
        }
        ++wakes;
        int pass_count = 0;
        const std::size_t got = drive(ctx, pass_count);
        const bool ready_now = racing.ready();
        std::printf("phase=drive t=%.3f delivered=%zu passes=%d ready=%d\n", ms(), got, pass_count,
                    int(ready_now));
        if (got > 0 && ready_now) {
            settled = true;
            break;
        }
    }
    signaler.join();
    for (int i = 0; i < 8 && [&] {
             int p = 0;
             return drive(ctx, p) > 0;
         }(); ++i) {
    }
    std::printf("phase=final t=%.3f settled=%d ready=%d wakes=%d\n", ms(), int(settled),
                int(racing.ready()), wakes);
    {
        std::lock_guard<std::mutex> lk(racing.mtx);
        racing.request.reset();
    }
    ctx.detach_progress_host();
    owner.reset();
    stop.store(true, std::memory_order_release);
    watcher.join();
    std::printf("verdict=%s\n", (settled && racing.ready()) ? "PASS" : "STALL");
    return (settled && racing.ready()) ? 0 : 1;
}
