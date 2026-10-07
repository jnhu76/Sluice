// Clean-room micro-contract probe: request-path metadata and durability,
// including resize coverage through a subsequent request sync without a
// conflicting mutation. Built against the installed prefix.
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

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
    char path[] = "/tmp/sluice_f1_cdur_XXXXXX";
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

sluice::FileOpen read_only() {
    sluice::FileOpen mode;
    mode.access = sluice::FileAccess::read_only;
    return mode;
}

} // namespace

int main() {
    using namespace sluice;
    using namespace sluice::async;

    // Metadata + identity through the request path: file_info and size return
    // the same observable facts as the direct path.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();
        const std::string payload = "metadata";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        CHECK(sluice::blocking::write_all(file, bytes).has_value());

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                                  .worker_count = 1}));
        auto info_req = ctx.submit_file_info(FileInfoOp{.file = NativeFileRef{file}});
        CHECK(info_req.has_value());
        while (!info_req.value().ready())
            (void)ctx.poll();
        const auto info = info_req.value().take_result();
        CHECK(info.result.has_value());
        CHECK(info.result.value().kind == FileKind::regular);
        CHECK(info.result.value().size == payload.size());
        CHECK(info.result.value().identity.has_value());

        auto size_req = ctx.submit_size(SizeOp{.file = NativeFileRef{file}});
        CHECK(size_req.has_value());
        while (!size_req.value().ready())
            (void)ctx.poll();
        const auto size = size_req.value().take_result();
        CHECK(size.result.has_value() && size.result.value() == FileSize{payload.size()});

        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    // Request-path half: direct resize (grow, then shrink) completed
    // before the sync request; the request-path sync_data/sync_all success
    // covers the file-size change; an independent reader observes it.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();
        const std::string payload = "grow";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        CHECK(sluice::blocking::write_all(file, bytes).has_value());

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                                  .worker_count = 1}));

        CHECK(sluice::blocking::resize(file, 128).has_value());
        auto sync_grow = ctx.submit_sync_data(SyncDataOp{.file = NativeFileRef{file}});
        CHECK(sync_grow.has_value());
        while (!sync_grow.value().ready())
            (void)ctx.poll();
        CHECK(sync_grow.value().take_result().result.has_value());
        CHECK(sluice::blocking::size(file).value() == 128);

        CHECK(sluice::blocking::resize(file, 2).has_value());
        auto sync_shrink = ctx.submit_sync_all(SyncAllOp{.file = NativeFileRef{file}});
        CHECK(sync_shrink.has_value());
        while (!sync_shrink.value().ready())
            (void)ctx.poll();
        CHECK(sync_shrink.value().take_result().result.has_value());

        auto verify = File::open(path, read_only());
        CHECK(verify.has_value());
        CHECK(sluice::blocking::size(verify.value()).value() == 2);
        CHECK(verify.value().close().has_value());

        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "contract_durability: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("contract_durability: PASS\n");
    return 0;
}
