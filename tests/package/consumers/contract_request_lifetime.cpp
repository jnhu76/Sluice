// Clean-room micro-contract probe: result/binding/identity lifecycle.
// Built against the installed prefix.
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
    char path[] = "/tmp/sluice_f1_crl_XXXXXX";
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
    const std::string payload = "request-lifetime";
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    CHECK(sluice::blocking::write_all(file, bytes).has_value());

    AsyncIoContext ctx(
        std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                              .worker_count = 1}));

    // Accepted, published, retained-but-unconsumed: ready() is observable
    // without consuming.
    std::vector<std::byte> buf(payload.size(), std::byte{0});
    auto r1 = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                     .dst = buf.data(),
                                     .len = buf.size(),
                                     .offset = 0});
    CHECK(r1.has_value());
    while (!r1.value().ready())
        (void)ctx.poll();
    const RequestId id1 = r1.value().id();
    CHECK(id1.valid());
    CHECK(ctx.lookup(id1) == RequestReadiness::ready);

    // try_result observes without consuming: the handle stays bound.
    const auto peek = r1.value().try_result();
    CHECK(peek.readiness == RequestReadiness::ready && peek.result.has_value() &&
          peek.result.value() == payload.size());
    CHECK(r1.value().valid());
    CHECK(ctx.lookup(id1) == RequestReadiness::ready);

    // take_result consumes exactly once and empties the handle.
    const auto taken = r1.value().take_result();
    CHECK(taken.readiness == RequestReadiness::ready && taken.result.has_value());
    CHECK(!r1.value().valid());
    // Stale identity: public lookup/cancel after release reports stale/not-found.
    CHECK(ctx.lookup(id1) == RequestReadiness::empty);
    const auto stale_cancel = ctx.cancel(id1);
    CHECK(stale_cancel.has_value());
    CHECK(stale_cancel.value() == CancelDisposition::not_found);

    // Move transfers responsibility; the moved-from handle is empty.
    auto r2 = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                     .dst = buf.data(),
                                     .len = buf.size(),
                                     .offset = 0});
    CHECK(r2.has_value());
    Request<std::size_t> moved = std::move(r2.value());
    CHECK(!r2.value().valid());
    CHECK(moved.valid());
    while (!moved.ready())
        (void)ctx.poll();

    // Self-move must not abandon accepted work.
    Request<std::size_t>& self = moved;
    moved = std::move(self);
    CHECK(moved.valid());
    while (!moved.ready())
        (void)ctx.poll();
    (void)moved.take_result();

    // discard() releases a published binding without consuming the payload.
    auto r3 = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                     .dst = buf.data(),
                                     .len = buf.size(),
                                     .offset = 0});
    CHECK(r3.has_value());
    while (!r3.value().ready())
        (void)ctx.poll();
    r3.value().discard();
    CHECK(!r3.value().valid());

    // Generation/identity: a fresh submission gets a fresh identity, and the
    // context keeps serving after repeated release cycles: released bindings
    // do not wedge reclaim.
    bool resubmit_ok = true;
    for (int i = 0; i < 3; ++i) {
        auto r = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                        .dst = buf.data(),
                                        .len = buf.size(),
                                        .offset = 0});
        if (!r.has_value()) {
            resubmit_ok = false;
            break;
        }
        while (!r.value().ready())
            (void)ctx.poll();
        (void)r.value().take_result();
    }
    CHECK(resubmit_ok);

    CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
    file.close();
    ::unlink(path.c_str());

    if (g_failures != 0) {
        std::fprintf(stderr, "contract_request_lifetime: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("contract_request_lifetime: PASS\n");
    return 0;
}
