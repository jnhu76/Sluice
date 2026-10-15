// Measure the wake latency of a zero-op submitted from a thread other than the
// blocked progress owner. Same source compiles against the F2 and the
// pre-F2 tree: the Request-based submit path exists in both.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

using namespace sluice::async;
using sluice::FileAccess;

namespace {

double now_ms() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
               .count() /
           1000.0;
}

bool fd_readable(int fd) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return ::poll(&p, 1, 0) > 0 && (p.revents & POLLIN) != 0;
}

Request<std::size_t> submit_zero_op(AsyncIoContext& ctx) {
    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), FileAccess::read_only),
                                            buffer.data(), 0, 0});
    if (!submitted.has_value())
        return {};
    return std::move(submitted).value();
}

int drive(AsyncIoContext& ctx) {
    ctx.acknowledge_progress_notification();
    int delivered = 0;
    for (int i = 0; i < 64; ++i) {
        const auto pass = ctx.poll_progress();
        if (!pass.has_value())
            break;
        delivered += static_cast<int>(pass.value().completed);
        if (pass.value().health_failed)
            break;
        if (pass.value().completed == 0 && !pass.value().immediate_work_remains)
            break;
    }
    return delivered;
}

} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 20;
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 1});
    AsyncIoContext ctx(std::move(backend));
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return 2;
    std::optional<ProgressOwner> owner = std::move(claimed).value();
    const int nfd = ctx.progress_notification_fd();

    std::vector<double> latencies;
    int timeouts = 0;
    int misses = 0;
    for (int round = 0; round < rounds; ++round) {
        Request<std::size_t> warm = submit_zero_op(ctx);
        if (!warm.valid())
            return 3;
        for (int i = 0; i < 2000 && !warm.ready(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        drive(ctx);
        warm.discard();
        while (fd_readable(nfd))
            drive(ctx);

        double parked_at = 0.0;
        double woke_at = 0.0;
        int poll_rc = -1;
        std::thread waiter([&] {
            parked_at = now_ms();
            struct pollfd p;
            p.fd = nfd;
            p.events = POLLIN;
            p.revents = 0;
            poll_rc = ::poll(&p, 1, 3000);
            woke_at = now_ms();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        const double submitted_at = now_ms();
        Request<std::size_t> late = submit_zero_op(ctx);
        if (!late.valid())
            return 4;
        waiter.join();
        if (poll_rc == 0)
            ++timeouts;
        else if (submitted_at > woke_at)
            ++misses;
        latencies.push_back(woke_at - submitted_at);
        drive(ctx);
        late.discard();
        while (fd_readable(nfd))
            drive(ctx);
    }
    double worst = -1e9;
    double total = 0;
    for (double value : latencies) {
        worst = value > worst ? value : worst;
        total += value;
    }
    std::printf("rounds=%d timeouts=%d prewake_misses=%d mean_ms=%.3f worst_ms=%.3f\n", rounds,
                timeouts, misses, total / static_cast<double>(latencies.size()), worst);
    for (std::size_t i = 0; i < latencies.size(); ++i)
        std::printf("  round=%zu latency_ms=%.3f\n", i, latencies[i]);
    ctx.detach_progress_host();
    owner.reset();
    return (timeouts == 0 && misses == 0) ? 0 : 1;
}
