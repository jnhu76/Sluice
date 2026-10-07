// Clean-room micro-contract probe: cancel dispositions and effect
// preservation. Built against the installed prefix.
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
    char path[] = "/tmp/sluice_f1_cce_XXXXXX";
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

bool direct_content_is(const std::string& path, const std::string& expected) {
    auto opened = sluice::File::open(path, [] {
        sluice::FileOpen mode;
        mode.access = sluice::FileAccess::read_only;
        return mode;
    }());
    if (!opened.has_value())
        return false;
    std::vector<std::byte> buf(expected.size() + 1, std::byte{0});
    const auto n = sluice::blocking::read_at(opened.value(), 0, buf);
    opened.value().close();
    return n.has_value() && n.value() == static_cast<std::ptrdiff_t>(expected.size()) &&
           std::string(reinterpret_cast<const char*>(buf.data()), expected.size()) == expected;
}

} // namespace

int main() {
    using namespace sluice;
    using namespace sluice::async;

    // Cancel racing a completed write: the confirmed count is preserved and no
    // rollback is fabricated.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();
        const std::string payload = "cancel-vs-success";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                                  .worker_count = 1}));
        auto r = ctx.submit_write(WriteOp{.file = NativeFileRef{file},
                                          .src = bytes.data(),
                                          .len = bytes.size(),
                                          .offset = 0});
        CHECK(r.has_value());
        const auto disposition = r.value().cancel();
        CHECK(disposition.has_value());
        switch (disposition.value()) {
        case CancelDisposition::won_before_execution:
        case CancelDisposition::requested:
        case CancelDisposition::already_terminal:
        case CancelDisposition::not_found:
        case CancelDisposition::physical_interruption_unsupported:
            break;
        }
        while (!r.value().ready())
            (void)ctx.poll();
        const auto observation = r.value().take_result();
        CHECK(observation.readiness == RequestReadiness::ready);
        if (observation.result.has_value()) {
            // Confirmed progress preserved; the effect report matches.
            CHECK(observation.result.value() == payload.size());
            CHECK(observation.effect.confirmed_bytes == payload.size());
            CHECK(direct_content_is(path, payload));
        } else {
            CHECK(observation.result.error().code == IoError::Code::canceled);
        }
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    // Pre-execution cancel on a zero-op request: the no-op publishes
    // immediately, so cancel reports already_terminal and the result stays a
    // successful zero — a cancel acknowledgment never rewrites a chosen
    // terminal.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                                  .worker_count = 1}));
        auto r = ctx.submit_read(
            ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
        CHECK(r.has_value());
        CHECK(r.value().ready());
        const auto disposition = r.value().cancel();
        CHECK(disposition.has_value());
        CHECK(disposition.value() == CancelDisposition::already_terminal);
        const auto observation = r.value().take_result();
        CHECK(observation.result.has_value() && observation.result.value() == 0);
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    // Request-path durability ordering: a sync request submitted after a
    // write request completed covers the confirmed bytes; both requests
    // settle successfully.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                                  .worker_count = 1}));
        const std::string payload = "ordered-write-sync";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        auto w = ctx.submit_write(WriteOp{.file = NativeFileRef{file},
                                          .src = bytes.data(),
                                          .len = bytes.size(),
                                          .offset = 0});
        CHECK(w.has_value());
        while (!w.value().ready())
            (void)ctx.poll();
        const auto wrote = w.value().take_result();
        CHECK(wrote.result.has_value() && wrote.result.value() == payload.size());

        auto s = ctx.submit_sync_data(SyncDataOp{.file = NativeFileRef{file}});
        CHECK(s.has_value());
        while (!s.value().ready())
            (void)ctx.poll();
        const auto synced = s.value().take_result();
        CHECK(synced.result.has_value());
        CHECK(direct_content_is(path, payload));
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "contract_cancel_effect: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("contract_cancel_effect: PASS\n");
    return 0;
}
