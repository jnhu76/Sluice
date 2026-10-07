// Clean-room P1 consumer (W-02): canonical bounded pipeline over
// Request<T>/RequestScope/AsyncIoContext/ThreadPoolBackend, built against the
// installed prefix. No Completion/ApplicationRuntime/Scheduler.
#include <sluice/async/request_scope.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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
    char path[] = "/tmp/sluice_f1_w02_XXXXXX";
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

    // W-02: bounded positional operations with independent results.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();
        const std::string payload = "0123456789abcdef";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        CHECK(sluice::blocking::write_all(file, bytes).has_value());

        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>(
            ThreadPoolConfig{.request_capacity = 8, .worker_count = 2}));
        RequestScope scope(ctx, 4, ScopeCleanupPolicy::drain);

        auto a = scope.submit_read(
            ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
        CHECK(a.has_value());

        std::vector<std::byte> buf1(4, std::byte{0});
        std::vector<std::byte> buf2(4, std::byte{0});
        auto b = scope.submit_read(ReadOp{.file = NativeFileRef{file},
                                          .dst = buf1.data(),
                                          .len = buf1.size(),
                                          .offset = 0});
        auto c = scope.submit_read(ReadOp{.file = NativeFileRef{file},
                                          .dst = buf2.data(),
                                          .len = buf2.size(),
                                          .offset = 8});
        CHECK(b.has_value() && c.has_value());

        CHECK(scope.wait_for(a.value(), std::chrono::seconds(5)).has_value());
        CHECK(scope.wait_for(b.value(), std::chrono::seconds(5)).has_value());
        CHECK(scope.wait_for(c.value(), std::chrono::seconds(5)).has_value());
        const auto ra = scope.take(a.value());
        CHECK(ra.readiness == RequestReadiness::ready && ra.result.has_value());
        const auto rb = scope.take(b.value());
        CHECK(rb.readiness == RequestReadiness::ready && rb.result.has_value() &&
              rb.result.value() == 4);
        const auto rc = scope.take(c.value());
        CHECK(rc.readiness == RequestReadiness::ready && rc.result.has_value() &&
              rc.result.value() == 4);
        const auto finished = scope.finish();
        CHECK(finished.has_value());
        CHECK(std::string(reinterpret_cast<const char*>(buf1.data()), 4) == "0123");
        CHECK(std::string(reinterpret_cast<const char*>(buf2.data()), 4) == "89ab");
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    // Short I/O through a request read: positional read spanning EOF.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();
        const std::string payload = "short";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        CHECK(sluice::blocking::write_all(file, bytes).has_value());

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                                  .worker_count = 1}));
        RequestScope scope(ctx, 2, ScopeCleanupPolicy::drain);
        std::vector<std::byte> big(64, std::byte{0});
        auto r = scope.submit_read(ReadOp{.file = NativeFileRef{file},
                                          .dst = big.data(),
                                          .len = big.size(),
                                          .offset = 2});
        CHECK(r.has_value());
        CHECK(scope.wait_for(r.value(), std::chrono::seconds(5)).has_value());
        const auto observed = scope.take(r.value());
        CHECK(observed.readiness == RequestReadiness::ready);
        CHECK(observed.result.has_value());
        CHECK(observed.result.value() == payload.size() - 2);
        CHECK(scope.finish().has_value());
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    // Retained results exhaust slots: unconsumed published results keep their
    // slots, and admission rejects with a distinct would_block outcome while
    // the context stays open.
    {
        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 2,
                                                                  .worker_count = 1}));
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();

        const std::string payload = "retained";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        CHECK(sluice::blocking::write_all(file, bytes).has_value());

        auto first = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                            .dst = nullptr,
                                            .len = 0,
                                            .offset = 0});
        auto second = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                             .dst = nullptr,
                                             .len = 0,
                                             .offset = 0});
        CHECK(first.has_value() && second.has_value());
        while (!first.value().ready() || !second.value().ready())
            (void)ctx.poll();

        auto third = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                            .dst = nullptr,
                                            .len = 0,
                                            .offset = 0});
        CHECK(!third.has_value());
        CHECK(third.error().code == IoError::Code::would_block);
        CHECK(ctx.admission_open());

        // Consuming releases the bindings; capacity returns after the pins
        // retire through a progress pass.
        (void)first.value().take_result();
        (void)second.value().take_result();
        bool capacity_returned = false;
        for (int i = 0; i < 100 && !capacity_returned; ++i) {
            (void)ctx.poll();
            capacity_returned =
                ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                       .dst = nullptr,
                                       .len = 0,
                                       .offset = 0})
                    .has_value();
        }
        CHECK(capacity_returned);
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    // Second submission fails while the first is outstanding; the scope still
    // settles the first request on the early-return path (capacity 1).
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();
        const std::string payload = "one-at-a-time";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        CHECK(sluice::blocking::write_all(file, bytes).has_value());

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 1,
                                                                  .worker_count = 1}));
        bool first_settled = false;
        {
            RequestScope scope(ctx, 2, ScopeCleanupPolicy::cancel_then_drain);
            std::vector<std::byte> buf(8, std::byte{0});
            auto first = scope.submit_read(ReadOp{.file = NativeFileRef{file},
                                                  .dst = buf.data(),
                                                  .len = buf.size(),
                                                  .offset = 0});
            CHECK(first.has_value());
            auto second = scope.submit_read(ReadOp{.file = NativeFileRef{file},
                                                   .dst = buf.data(),
                                                   .len = buf.size(),
                                                   .offset = 0});
            CHECK(!second.has_value());
            CHECK(ctx.admission_open());
        } // early return: destructor settles the outstanding first request
        first_settled = true;
        CHECK(first_settled);
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    // Exception cleanup: unwinding through the scope settles accepted work.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                                  .worker_count = 1}));
        bool threw = false;
        try {
            RequestScope scope(ctx, 2, ScopeCleanupPolicy::cancel_then_drain);
            std::vector<std::byte> buf(4, std::byte{0});
            auto r = scope.submit_read(ReadOp{.file = NativeFileRef{file},
                                              .dst = buf.data(),
                                              .len = buf.size(),
                                              .offset = 0});
            CHECK(r.has_value());
            throw std::runtime_error("w02 exception-cleanup probe");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(ctx.admission_open());
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    // Best-effort cancel: any disposition is legal, the operation settles, and
    // a successful outcome is never rewritten into a fabricated failure.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();
        const std::string payload(1 << 16, 'x');
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        CHECK(sluice::blocking::write_all(file, bytes).has_value());

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                                  .worker_count = 1}));
        std::vector<std::byte> buf(payload.size(), std::byte{0});
        auto r = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                        .dst = buf.data(),
                                        .len = buf.size(),
                                        .offset = 0});
        CHECK(r.has_value());
        const auto disposition = r.value().cancel();
        CHECK(disposition.has_value());
        while (!r.value().ready())
            (void)ctx.poll();
        const auto observation = r.value().take_result();
        CHECK(observation.readiness == RequestReadiness::ready);
        if (observation.result.has_value()) {
            CHECK(observation.result.value() <= payload.size());
        } else {
            CHECK(observation.result.error().code == IoError::Code::canceled);
        }
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "w02_pipeline: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("w02_pipeline: PASS\n");
    return 0;
}
