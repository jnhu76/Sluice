// Clean-room micro-contract probe: shutdown with retained results.
// Fail-fast and poison paths are process-fatal or fault-seam-only and cannot
// be probed in-process; they map to the internal shutdown oracles in the F1
// evidence matrix.
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
    char path[] = "/tmp/sluice_f1_cshut_XXXXXX";
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
    const std::string payload = "shutdown-retained";
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    CHECK(sluice::blocking::write_all(file, bytes).has_value());

    AsyncIoContext ctx(
        std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                              .worker_count = 1}));

    // Ready-but-unconsumed results survive execution close.
    std::vector<std::byte> buf(payload.size(), std::byte{0});
    auto r = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                    .dst = buf.data(),
                                    .len = buf.size(),
                                    .offset = 0});
    CHECK(r.has_value());
    while (!r.value().ready())
        (void)ctx.poll();

    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    CHECK(outcome.has_value());
    CHECK(outcome.value() == ShutdownOutcome::completed);
    CHECK(ctx.execution_closed());
    CHECK(!ctx.admission_open());

    // The retained result stays consumable after execution close; no
    // consumption deadlock.
    const auto observation = r.value().take_result();
    CHECK(observation.readiness == RequestReadiness::ready);
    CHECK(observation.result.has_value() && observation.result.value() == payload.size());
    CHECK(std::string(reinterpret_cast<const char*>(buf.data()), buf.size()) == payload);

    // Re-shutdown is idempotent and reports the same completion.
    const auto again = ctx.shutdown(ShutdownPolicy::drain);
    CHECK(again.has_value());
    CHECK(again.value() == ShutdownOutcome::completed);

    file.close();
    ::unlink(path.c_str());

    if (g_failures != 0) {
        std::fprintf(stderr, "contract_shutdown: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("contract_shutdown: PASS\n");
    return 0;
}
