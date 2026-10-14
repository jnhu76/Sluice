// F6 clean-room W-02 consumer: bounded pipeline through AsyncIoContext +
// RequestScope on the ThreadPool backend; second-submission rejection with
// the first outstanding is exercised at scope capacity 1.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

int main() {
    using sluice::File;
    using sluice::FileAccess;
    using sluice::FileExistence;
    using sluice::FileInitialContents;
    using sluice::FileOpen;
    namespace sa = sluice::async;

    const char* path = "/tmp/f6_w02_probe.dat";
    std::vector<std::byte> payload(13);
    std::memcpy(payload.data(), "w02-payload!!", 13);
    {
        auto f = File::open(path, FileOpen{
            FileAccess::write_only, FileExistence::create_if_missing,
            FileInitialContents::truncate});
        assert(f.has_value());
        auto w = sluice::blocking::write_at(f.value(), 0, payload);
        assert(w.has_value());
    }

    sa::AsyncIoContext ctx(std::make_unique<sa::ThreadPoolBackend>(
        sa::ThreadPoolConfig{.request_capacity = 1, .worker_count = 2}));
    {
        sa::RequestScope scope(ctx, 1, sa::ScopeCleanupPolicy::drain);
        auto f = File::open(path);
        assert(f.has_value());
        std::vector<std::byte> a(13, std::byte{0}), b(13, std::byte{0});

        auto t1 = scope.submit_read(sa::ReadOp{sa::NativeFileRef{f.value()},
                                               a.data(), a.size(), 0});
        assert(t1.has_value());
        auto t2 = scope.submit_read(sa::ReadOp{sa::NativeFileRef{f.value()},
                                               b.data(), b.size(), 0});
        assert(!t2.has_value());

        auto waited = scope.wait_for(t1.value(), std::chrono::seconds(5));
        assert(waited.has_value());
        assert(waited.value() == sa::ScopeWaitStatus::ready);
        auto obs = scope.take(t1.value());
        assert(obs.readiness == sa::RequestReadiness::ready);
        assert(obs.result.has_value() && obs.result.value() == 13);
        assert(std::memcmp(a.data(), "w02-payload!!", 13) == 0);
        auto finish = scope.finish();
        assert(finish.has_value());
    }
    ctx.close_admission();
    auto shutdown = ctx.shutdown(sa::ShutdownPolicy::drain);
    assert(shutdown.has_value());
    assert(shutdown.value() == sa::ShutdownOutcome::completed);

    std::remove(path);
    std::puts("F6_W02_SCOPE_CONSUMER_OK");
    return 0;
}
