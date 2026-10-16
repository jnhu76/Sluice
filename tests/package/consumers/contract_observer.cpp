// Clean-room micro-contract probe: observer attachment, cancellation and
// the public retirement query, driven through the copied RequestId
// identity. The terminal notification runs on the driver-owned hook; an
// external consumer observes the terminal itself through polling and the
// completed delivery as a retirement that already reports acquired.
#include <sluice/async/async_io_context.hpp>
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
    char path[] = "/tmp/sluice_v2_cobs_XXXXXX";
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
    const std::string payload = "observer";
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    CHECK(sluice::blocking::write_all(file, bytes).has_value());

    AsyncIoContext ctx = make_threadpool_context(
        ThreadPoolConfig{.request_capacity = 4, .worker_count = 1});

    // Terminal before attach: the attachment resolves atomically as
    // already_terminal; the notification is never lost.
    {
        auto submitted = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                .dst = nullptr,
                                                .len = 0,
                                                .offset = 0});
        CHECK(submitted.has_value());
        Request<std::size_t> request = std::move(submitted).value();
        while (!request.ready())
            (void)ctx.poll();
        const auto attachment = ctx.attach_observer(request.id());
        CHECK(attachment.status == AsyncIoContext::ObserverStatus::already_terminal);
        CHECK(request.take_result().result.has_value());
    }

    // Attach on an accepted-not-yet-published request: armed, exactly one
    // future terminal delivery. The driver runs and retires that delivery
    // on the pass that publishes the terminal, so once polling observes the
    // terminal the retirement query already reports acquired; the
    // attachment resolves atomically so a request that published first
    // reports already_terminal instead.
    {
        std::vector<std::byte> buf(4, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                .dst = buf.data(),
                                                .len = buf.size(),
                                                .offset = 0});
        CHECK(submitted.has_value());
        Request<std::size_t> request = std::move(submitted).value();
        const auto attachment = ctx.attach_observer(request.id());
        CHECK(attachment.status == AsyncIoContext::ObserverStatus::armed ||
              attachment.status == AsyncIoContext::ObserverStatus::already_terminal);
        while (!request.ready())
            (void)ctx.poll();
        if (attachment.status == AsyncIoContext::ObserverStatus::armed) {
            CHECK(attachment.armed());
            CHECK(attachment.request == request.id());
            bool retired = ctx.delivery_retired(attachment.request);
            for (int i = 0; i < 100 && !retired; ++i) {
                (void)ctx.poll();
                retired = ctx.delivery_retired(attachment.request);
            }
            CHECK(retired);
            (void)ctx.poll();
            CHECK(ctx.delivery_retired(attachment.request));
        }
        CHECK(request.take_result().result.has_value());
    }

    // Cancel registration before publication: delivery is suppressed and the
    // registration retires. A delivery already claimed stays in progress
    // until the driver finishes it; the caller then acquires retirement by
    // re-cancelling.
    {
        std::vector<std::byte> buf(4, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                .dst = buf.data(),
                                                .len = buf.size(),
                                                .offset = 0});
        CHECK(submitted.has_value());
        Request<std::size_t> request = std::move(submitted).value();
        const auto attachment = ctx.attach_observer(request.id());
        CHECK(attachment.status == AsyncIoContext::ObserverStatus::armed ||
              attachment.status == AsyncIoContext::ObserverStatus::already_terminal);
        auto cancelled = ctx.cancel_observer(request.id());
        CHECK(cancelled.status == AsyncIoContext::ObserverCancelStatus::retired ||
              cancelled.status == AsyncIoContext::ObserverCancelStatus::delivery_in_progress);
        CHECK(cancelled.request == request.id());
        for (int i = 0;
             i < 100 && cancelled.status == AsyncIoContext::ObserverCancelStatus::delivery_in_progress;
             ++i) {
            (void)ctx.poll();
            const auto retry = ctx.cancel_observer(request.id());
            if (retry.status == AsyncIoContext::ObserverCancelStatus::retired)
                cancelled.status = retry.status;
        }
        CHECK(cancelled.status == AsyncIoContext::ObserverCancelStatus::retired);
        while (!request.ready())
            (void)ctx.poll();
        CHECK(request.take_result().result.has_value());
    }

    // One registration per request: a second attachment reports duplicate
    // disposition.
    {
        std::vector<std::byte> buf(4, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                .dst = buf.data(),
                                                .len = buf.size(),
                                                .offset = 0});
        CHECK(submitted.has_value());
        Request<std::size_t> request = std::move(submitted).value();
        const auto first = ctx.attach_observer(request.id());
        const auto second = ctx.attach_observer(request.id());
        CHECK(first.status == AsyncIoContext::ObserverStatus::armed ||
              first.status == AsyncIoContext::ObserverStatus::already_terminal);
        CHECK(second.status == AsyncIoContext::ObserverStatus::duplicate);
        CHECK(!second.armed());
        (void)ctx.cancel_observer(request.id());
        while (!request.ready())
            (void)ctx.poll();
        CHECK(request.take_result().result.has_value());
    }

    // Unknown identity: an id that was never registered (or already retired)
    // reports not_found without disturbing live registrations.
    {
        RequestId unknown{};
        const auto attachment = ctx.attach_observer(unknown);
        CHECK(attachment.status == AsyncIoContext::ObserverStatus::not_found);
        const auto cancelled = ctx.cancel_observer(unknown);
        CHECK(cancelled.status == AsyncIoContext::ObserverCancelStatus::not_found);
    }

    // Binding release after a delivered registration: the retirement is
    // acquired while the binding is still live, and consuming the result
    // afterwards is legal; the released identity may stop resolving once the
    // slot reclaims.
    {
        std::vector<std::byte> buf(4, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                .dst = buf.data(),
                                                .len = buf.size(),
                                                .offset = 0});
        CHECK(submitted.has_value());
        Request<std::size_t> request = std::move(submitted).value();
        const auto attachment = ctx.attach_observer(request.id());
        CHECK(attachment.status == AsyncIoContext::ObserverStatus::armed ||
              attachment.status == AsyncIoContext::ObserverStatus::already_terminal);
        while (!request.ready())
            (void)ctx.poll();
        bool retired = false;
        for (int i = 0; i < 100 && !retired; ++i) {
            retired = ctx.delivery_retired(attachment.request);
            if (!retired)
                (void)ctx.poll();
        }
        CHECK(retired);
        CHECK(request.take_result().result.has_value());
    }

    CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
    file.close();
    ::unlink(path.c_str());

    if (g_failures != 0) {
        std::fprintf(stderr, "contract_observer: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("contract_observer: PASS\n");
    return 0;
}
