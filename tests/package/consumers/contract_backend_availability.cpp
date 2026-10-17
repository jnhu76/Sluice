// Clean-room micro-contract probe: backend selection through the named
// factories. Selection succeeds or fails explicitly; an unavailable backend
// is reported as not_supported and never falls back silently to another
// execution. Built against the installed prefix; no backend class, seam or
// macro view is visible to this TU.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
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
    char path[] = "/tmp/sluice_v2_cba_XXXXXX";
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

// Drives one admitted request to completion through the public surface:
// availability of a selected backend is observed through the request profile
// itself, not through backend introspection.
bool roundtrip(sluice::async::AsyncIoContext& ctx, sluice::File& file) {
    using namespace sluice::async;
    auto submitted = ctx.submit_read(
        ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted).value();
    while (!request.ready())
        (void)ctx.poll();
    const auto observed = request.take_result();
    if (observed.readiness != RequestReadiness::ready || !observed.result.has_value())
        return false;
    return ctx.shutdown(ShutdownPolicy::drain).has_value();
}

} // namespace

int main() {
    using namespace sluice;
    using namespace sluice::async;

    const std::string path = make_temp_path();
    CHECK(!path.empty());
    auto opened = File::open(path, create_read_write());
    CHECK(opened.has_value());
    File file = std::move(opened).value();

    // Named ThreadPool selection: the required profile. Capacity is observed
    // through admission: capacity 2 admits two retained results and rejects
    // the third with would_block while the context stays open.
    {
        AsyncIoContext ctx = make_threadpool_context(
            ThreadPoolConfig{.request_capacity = 2, .worker_count = 1});
        auto a = ctx.submit_read(
            ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
        auto b = ctx.submit_read(
            ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
        CHECK(a.has_value() && b.has_value());
        while (!a.value().ready() || !b.value().ready())
            (void)ctx.poll();
        auto c = ctx.submit_read(
            ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
        CHECK(!c.has_value());
        CHECK(c.error().code == IoError::Code::would_block);
        CHECK(ctx.admission_open());
        (void)a.value().take_result();
        (void)b.value().take_result();
        bool capacity_returned = false;
        for (int i = 0; i < 100 && !capacity_returned; ++i) {
            (void)ctx.poll();
            capacity_returned = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                       .dst = nullptr,
                                                       .len = 0,
                                                       .offset = 0})
                                     .has_value();
        }
        CHECK(capacity_returned);
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
    }

    // ThreadPool selection with an invalid configuration fails explicitly:
    // the factory throws; no silently repaired configuration is constructed.
    {
        bool threw = false;
        try {
            AsyncIoContext ctx = make_threadpool_context(
                ThreadPoolConfig{.request_capacity = 0, .worker_count = 1});
            (void)ctx;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    // Named io_uring selection. Without the liburing profile the factory
    // reports the explicit not_supported failure and constructs nothing; with
    // the profile, construction-time ring/capability setup failures throw as
    // documented — either way there is no silent fallback to ThreadPool.
    {
        bool setup_threw = false;
        Result<AsyncIoContext> selected =
            make_unexpected<AsyncIoContext>(IoError{IoError::Code::not_supported});
        try {
            selected = make_uring_context(
                UringConfig{.request_capacity = 8, .queue_depth = 16});
        } catch (const std::exception&) {
            setup_threw = true;
        }
        if (setup_threw) {
            std::printf(
                "contract_backend_availability: uring runtime-unavailable (explicit)\n");
        } else if (selected.has_value()) {
            CHECK(roundtrip(selected.value(), file));
            std::printf("contract_backend_availability: uring selected\n");
        } else {
            CHECK(selected.error().code == IoError::Code::not_supported);
            std::printf(
                "contract_backend_availability: uring not_supported (explicit)\n");
        }

        // The not_supported outcome must not have been a fallback: a
        // subsequent ThreadPool selection from the same TU still constructs
        // and serves requests.
        AsyncIoContext tp = make_threadpool_context(
            ThreadPoolConfig{.request_capacity = 4, .worker_count = 1});
        CHECK(roundtrip(tp, file));
    }

    file.close();
    ::unlink(path.c_str());

    if (g_failures != 0) {
        std::fprintf(stderr, "contract_backend_availability: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("contract_backend_availability: PASS\n");
    return 0;
}
