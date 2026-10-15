#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace sluice::async;

bool notification_fd_readable(int fd) {
    if (fd < 0)
        return false;
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    const int rc = ::poll(&p, 1, 0);
    return rc > 0 && (p.revents & POLLIN) != 0;
}

struct HostInterest {
    AsyncIoContext& ctx;
    const int fd;
    explicit HostInterest(AsyncIoContext& c) noexcept
        : ctx(c), fd(c.progress_notification_fd()) {}
    ~HostInterest() { ctx.detach_progress_host(); }
};

Request<std::size_t> submit_zero_op(AsyncIoContext& ctx) {
    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), sluice::FileAccess::read_only),
                               buffer.data(), 0, 0});
    if (!submitted.has_value())
        return {};
    return std::move(submitted).value();
}

bool all_ready(const std::vector<Request<std::size_t>>& requests) {
    for (const auto& r : requests) {
        if (!r.ready())
            return false;
    }
    return true;
}

bool wait_all_ready(AsyncIoContext& ctx, std::vector<Request<std::size_t>>& requests) {
    const auto bound = std::chrono::seconds{8};
    while (!all_ready(requests)) {
        auto woke = ctx.wait_one(bound);
        if (!woke.has_value())
            return false;
        if (woke.value().kind != AsyncIoContext::ProgressWaitOutcome::Kind::progress)
            return false;
    }
    return true;
}

// The first notification byte must reach the fd; further notifications
// without an acknowledge must keep it readable without another byte; the
// acknowledge must empty it; and the next notification must reach it again —
// the last step fails if a suppressed write were skipped while the summary
// bit stayed set past its EAGAIN.
bool coalesce_then_acknowledge_round_trip() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    if (interest.fd < 0)
        return false;

    std::vector<Request<std::size_t>> requests;
    for (int n = 0; n < 3; ++n) {
        auto r = submit_zero_op(ctx);
        if (!r.valid()) {
            std::fprintf(stderr, "case0: submit %d invalid\n", n);
            return false;
        }
        requests.push_back(std::move(r));
        if (!notification_fd_readable(interest.fd)) {
            std::fprintf(stderr, "case0: fd unreadable after submit %d\n", n);
            return false;
        }
    }

    ctx.acknowledge_progress_notification();
    if (notification_fd_readable(interest.fd))
        return false;

    auto again = submit_zero_op(ctx);
    if (!again.valid())
        return false;
    requests.push_back(std::move(again));
    const bool reachable_again = notification_fd_readable(interest.fd);

    if (!wait_all_ready(ctx, requests))
        return false;
    for (auto& r : requests)
        r.discard();
    ctx.acknowledge_progress_notification();
    return reachable_again && !notification_fd_readable(interest.fd);
}

// Concurrent producers share the summary under the source mutex, so no
// completion may be stranded: the driver must observe every request ready
// within a bounded wait and the acknowledge must leave the fd empty.
bool concurrent_producers_never_strand_a_completion() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    if (interest.fd < 0)
        return false;

    constexpr int kProducers = 4;
    std::vector<Request<std::size_t>> requests;
    {
        std::mutex produce_mtx;
        std::vector<std::thread> producers;
        for (int n = 0; n < kProducers; ++n) {
            producers.emplace_back([&] {
                auto r = submit_zero_op(ctx);
                std::lock_guard<std::mutex> lk(produce_mtx);
                if (r.valid())
                    requests.push_back(std::move(r));
            });
        }
        for (auto& t : producers)
            t.join();
    }
    if (static_cast<int>(requests.size()) != kProducers)
        return false;

    if (!wait_all_ready(ctx, requests))
        return false;
    for (auto& r : requests)
        r.discard();
    ctx.acknowledge_progress_notification();
    return !notification_fd_readable(interest.fd);
}

} // namespace

int main() {
    using Fn = bool (*)();
    const Fn cases[] = {
        coalesce_then_acknowledge_round_trip,
        concurrent_producers_never_strand_a_completion,
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!cases[i]()) {
            std::fprintf(stderr, "progress_notification_suppression_test: case %zu failed\n",
                         i);
            return 1;
        }
    }
    return 0;
}
