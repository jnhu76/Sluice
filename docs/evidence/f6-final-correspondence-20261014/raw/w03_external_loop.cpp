// F6 clean-room W-03 consumer: application-owned poll(2) external loop over
// the context notification fd (PROG-03); no Scheduler/Fiber, no busy polling.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <poll.h>
#include <vector>

int main() {
    using sluice::File;
    using sluice::FileAccess;
    using sluice::FileExistence;
    using sluice::FileInitialContents;
    using sluice::FileOpen;
    namespace sa = sluice::async;

    const char* path = "/tmp/f6_w03_probe.dat";
    std::vector<std::byte> payload(13);
    std::memcpy(payload.data(), "w03-external", 13);
    {
        auto f = File::open(path, FileOpen{
            FileAccess::write_only, FileExistence::create_if_missing,
            FileInitialContents::truncate});
        assert(f.has_value());
        auto w = sluice::blocking::write_at(f.value(), 0, payload);
        assert(w.has_value());
    }

    sa::AsyncIoContext ctx(std::make_unique<sa::ThreadPoolBackend>(
        sa::ThreadPoolConfig{.request_capacity = 4, .worker_count = 1}));
    int fd = ctx.progress_notification_fd();
    assert(fd >= 0);
    auto f = File::open(path);
    assert(f.has_value());
    std::vector<std::byte> buf(13, std::byte{0});

    auto req = ctx.submit_read(sa::ReadOp{sa::NativeFileRef{f.value()},
                                          buf.data(), buf.size(), 0});
    assert(req.has_value());

    for (int spins = 0; spins < 2000 && !req.value().ready(); ++spins) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, 1000);
        assert(rc >= 0);
        ctx.acknowledge_progress_notification();
        auto completed = ctx.poll();
        (void)completed;
    }
    assert(req.value().ready());
    auto obs = req.value().take_result();
    assert(obs.readiness == sa::RequestReadiness::ready);
    assert(obs.result.has_value() && obs.result.value() == 13);
    assert(std::memcmp(buf.data(), "w03-external", 13) == 0);

    ctx.detach_progress_host();
    ctx.close_admission();
    auto shutdown = ctx.shutdown(sa::ShutdownPolicy::drain);
    assert(shutdown.has_value());
    std::remove(path);
    std::puts("F6_W03_EXTERNAL_LOOP_CONSUMER_OK");
    return 0;
}
