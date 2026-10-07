// Clean-room micro-contract probe: observer attachment, delivery and
// cancellation. The delivery vehicle is the current public spelling
// (Completion-based attach on the context).
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
    char path[] = "/tmp/sluice_f1_cobs_XXXXXX";
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

struct RecordingSink : sluice::async::detail::SynchronousReadySink {
    std::vector<sluice::async::detail::ReadyEvent> events;
    void on_ready(sluice::async::detail::ReadyEvent event) noexcept override {
        events.push_back(event);
    }
};

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

    AsyncIoContext ctx(
        std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 4,
                                                              .worker_count = 1}));
    RecordingSink sink;
    ctx.set_ready_sink(&sink);

    // Terminal before attach: the attachment resolves atomically as
    // already-terminal; the notification is never lost.
    {
        Completion<std::size_t> completion;
        auto submitted = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                .dst = nullptr,
                                                .len = 0,
                                                .offset = 0},
                                         completion);
        CHECK(submitted.has_value());
        while (!completion.ready())
            (void)ctx.poll();
        const auto attachment = ctx.attach_observer(completion);
        CHECK(attachment.status == sluice::async::detail::ObserverRegistration::already_terminal);
        CHECK(completion.result().has_value());
        completion.reset();
    }

    // Attach on an accepted-not-yet-published request: armed, exactly one
    // future terminal event, delivered by the driver's progress pass; the
    // attachment resolves atomically so a completion that published first
    // reports already-terminal instead.
    {
        Completion<std::size_t> completion;
        std::vector<std::byte> buf(4, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                .dst = buf.data(),
                                                .len = buf.size(),
                                                .offset = 0},
                                         completion);
        CHECK(submitted.has_value());
        const auto attachment = ctx.attach_observer(completion);
        CHECK(attachment.status == sluice::async::detail::ObserverRegistration::armed ||
              attachment.status ==
                  sluice::async::detail::ObserverRegistration::already_terminal);
        const std::size_t events_before = sink.events.size();
        while (!completion.ready())
            (void)ctx.poll();
        if (attachment.status == sluice::async::detail::ObserverRegistration::armed) {
            // Delivery rides the driver's progress pass; it may already have
            // fired in the pass that published the terminal.
            bool delivered = sink.events.size() > events_before;
            for (int i = 0; i < 100 && !delivered; ++i) {
                (void)ctx.poll();
                delivered = sink.events.size() > events_before;
            }
            CHECK(delivered);
            const auto retirement = ctx.retire_delivery(attachment.key);
            CHECK(retirement);
        }
        CHECK(completion.result().has_value());
        completion.reset();
    }

    // Cancel registration before publication: delivery is suppressed and the
    // registration retires. A delivery already claimed stays
    // in progress until the driver finishes it; the caller then acquires
    // retirement by re-cancelling.
    {
        Completion<std::size_t> completion;
        std::vector<std::byte> buf(4, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                .dst = buf.data(),
                                                .len = buf.size(),
                                                .offset = 0},
                                         completion);
        CHECK(submitted.has_value());
        const auto attachment = ctx.attach_observer(completion);
        CHECK(attachment.status == sluice::async::detail::ObserverRegistration::armed ||
              attachment.status ==
                  sluice::async::detail::ObserverRegistration::already_terminal);
        auto cancelled = ctx.cancel_observer(completion);
        CHECK(cancelled.status == sluice::async::detail::ObserverCancellation::retired ||
              cancelled.status ==
                  sluice::async::detail::ObserverCancellation::delivery_in_progress);
        for (int i = 0;
             i < 100 &&
             cancelled.status == sluice::async::detail::ObserverCancellation::delivery_in_progress;
             ++i) {
            (void)ctx.poll();
            const auto retry = ctx.cancel_observer(completion);
            if (retry.status == sluice::async::detail::ObserverCancellation::retired)
                cancelled.status = retry.status;
        }
        while (!completion.ready())
            (void)ctx.poll();
        CHECK(completion.result().has_value());
        completion.reset();
    }

    // One registration per request: a second attachment reports duplicate
    // occupied disposition.
    {
        Completion<std::size_t> completion;
        std::vector<std::byte> buf(4, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                .dst = buf.data(),
                                                .len = buf.size(),
                                                .offset = 0},
                                         completion);
        CHECK(submitted.has_value());
        auto first = ctx.attach_observer(completion);
        auto second = ctx.attach_observer(completion);
        CHECK(first.status == sluice::async::detail::ObserverRegistration::armed ||
              first.status ==
                  sluice::async::detail::ObserverRegistration::already_terminal);
        CHECK(second.status == sluice::async::detail::ObserverRegistration::duplicate);
        (void)ctx.cancel_observer(completion);
        while (!completion.ready())
            (void)ctx.poll();
        CHECK(completion.result().has_value());
        completion.reset();
    }

    // Binding release while a delivery remains: the result is consumed (public
    // binding released) while the delivery bookkeeping is still retiring; the
    // delivery cannot consume the released result and the state stays
    // queryable until retired.
    {
        Completion<std::size_t> completion;
        std::vector<std::byte> buf(4, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{.file = NativeFileRef{file},
                                                .dst = buf.data(),
                                                .len = buf.size(),
                                                .offset = 0},
                                         completion);
        CHECK(submitted.has_value());
        const auto attachment = ctx.attach_observer(completion);
        CHECK(attachment.status == sluice::async::detail::ObserverRegistration::armed ||
              attachment.status ==
                  sluice::async::detail::ObserverRegistration::already_terminal);
        while (!completion.ready())
            (void)ctx.poll();
        CHECK(completion.result().has_value());
        completion.reset();
        for (int i = 0; i < 100 && !ctx.retire_delivery(attachment.key); ++i)
            (void)ctx.poll();
    }

    ctx.set_ready_sink(nullptr);
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
