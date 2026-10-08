#include <sluice/async/async_io_context.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <liburing.h>

#include <csignal>

namespace {

using namespace sluice::async;

bool notification_fd_readable(int fd) {
    if (fd < 0)
        return false;
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    const int rc = ::poll(&p, 1, 0);
    return rc > 0 && (p.revents & POLLIN) != 0;
}

// Test-owned teardown record: the hook wraps the real unregister so the
// assertions survive backend destruction and observe the real syscall result.
struct TeardownRecord {
    std::atomic<int> unregisters{0};
    std::atomic<int> rc_failures{0};
};

int recording_unregister(void* context, ::io_uring* ring) noexcept {
    auto* record = static_cast<TeardownRecord*>(context);
    record->unregisters.fetch_add(1, std::memory_order_relaxed);
    const int rc = ::io_uring_unregister_eventfd(ring);
    if (rc != 0)
        record->rc_failures.fetch_add(1, std::memory_order_relaxed);
    return rc;
}

// A standalone backend performs no kernel registration: the notification
// binding is installed at context attachment only.
bool ring_setup_available() {
    try {
        return UringAsyncBackend().available();
    } catch (...) {
        return false;
    }
}

bool standalone_backend_registers_nothing() {
    if (!ring_setup_available())
        return true;
    UringAsyncBackend backend;
    return backend.eventfd_registrations_for_test() == 0 &&
           backend.eventfd_unregistrations_for_test() == 0;
}

// Successful attachment registers the context notification fd with the real
// kernel ring exactly once, and the registered wiring carries a real kernel
// completion to the context fd.
bool attachment_registers_kernel_notification_once() {
    TeardownRecord teardown;
    UringBackendSubmitTestHooks hooks;
    hooks.context = &teardown;
    hooks.unregister_eventfd = &recording_unregister;
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{16, 8}, hooks);
    UringAsyncBackend* raw = backend.get();
    {
        AsyncIoContext ctx(std::move(backend));
        if (!ctx.has_split_wait_capability())
            return false;
        if (raw->eventfd_registrations_for_test() != 1)
            return false;
        if (raw->eventfd_unregistrations_for_test() != 0)
            return false;

        const std::string content(64, 'r');
        char path[] = "/tmp/sluice_uring_reg_XXXXXX";
        const int fd = ::mkstemp(path);
        if (fd < 0)
            return false;
        ::unlink(path);
        if (::write(fd, content.data(), content.size()) !=
            static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return false;
        }
        ::lseek(fd, 0, SEEK_SET);

        std::vector<std::byte> buffer(32, std::byte{0});
        Request<std::size_t> c;
        auto read_submitted =
            ctx.submit_read(ReadOp{NativeFileRef(fd, sluice::FileAccess::read_only),
                                   buffer.data(), buffer.size(), 0});
        if (!read_submitted.has_value()) {
            ::close(fd);
            return false;
        }
        c = std::move(read_submitted).value();
        // The public binding forbids nonterminal release: a defect exit drains
        // the in-flight read to publication before the binding goes.
        struct RequestPublicationDrain {
            AsyncIoContext& ctx;
            Request<std::size_t>& request;
            ~RequestPublicationDrain() {
                for (int i = 0; i < 200000 && request.valid() && !request.ready(); ++i)
                    (void)ctx.poll();
                request.discard();
            }
        } drain{ctx, c};
        // Dispatching accepted work is the driver's obligation; run the
        // submission pass, then the registered eventfd must carry the kernel
        // completion to the context fd. The submitting pass can also be the
        // servicing pass, so its delivery joins the wait's count.
        const auto driven_pass = ctx.poll_progress();
        const std::size_t driven =
            driven_pass.has_value() ? driven_pass.value().completed : 0;
        bool woken = notification_fd_readable(ctx.progress_notification_fd());
        for (int i = 0; i < 20000 && !woken; ++i) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            woken = notification_fd_readable(ctx.progress_notification_fd());
        }
        const auto waited = ctx.wait_one(std::chrono::milliseconds{5000});
        ::close(fd);
        using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
        const std::size_t waited_completed =
            waited.has_value() && waited.value().kind == WaitKind::progress
                ? waited.value().completed
                : 0;
        if (!woken || !waited.has_value() || driven + waited_completed != 1 || !c.ready())
            return false;
        c.discard();
        ctx.detach_progress_host();
    }
    // Teardown order: unregister runs once while the ring still exists, before
    // ring exit, and the backend retires inside the context.
    return teardown.unregisters.load() == 1 && teardown.rc_failures.load() == 0;
}

namespace {
struct ForcedFailureState {
    std::atomic<int> calls{0};
};

int forced_register_failure(void* context, ::io_uring*, int) noexcept {
    static_cast<ForcedFailureState*>(context)->calls.fetch_add(1, std::memory_order_relaxed);
    return -1;
}
}

// §9: if the real ring cannot install its required ProgressSource notification
// binding, the io_uring profile is not usable — the context construction fails
// explicitly instead of falling back to any ring-fd host waiting.
bool forced_registration_failure_fails_context_construction() {
    ForcedFailureState state;
    UringBackendSubmitTestHooks hooks;
    hooks.context = &state;
    hooks.register_eventfd = &forced_register_failure;

    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 8}, hooks);
    bool construction_failed = false;
    try {
        AsyncIoContext ctx(std::move(backend));
        (void)ctx;
    } catch (const std::runtime_error&) {
        construction_failed = true;
    } catch (...) {
        return false;
    }
    return construction_failed && state.calls.load(std::memory_order_relaxed) == 1;
}

// §22 narrow teardown evidence: registered → context teardown → unregister
// before ring exit; no double registration, no dangling kernel registration.
bool teardown_unregisters_before_ring_exit() {
    TeardownRecord teardown;
    UringBackendSubmitTestHooks hooks;
    hooks.context = &teardown;
    hooks.unregister_eventfd = &recording_unregister;
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 8}, hooks);
    UringAsyncBackend* raw = backend.get();
    unsigned registrations = 0;
    {
        AsyncIoContext ctx(std::move(backend));
        registrations = raw->eventfd_registrations_for_test();
    }
    return registrations == 1 && teardown.unregisters.load() == 1 &&
           teardown.rc_failures.load() == 0;
}

}

int main() {
    ::alarm(120);
    if (!ring_setup_available()) {
        std::printf("all 0 uring registration lifecycle tests passed (ring unavailable)\n");
        return 0;
    }
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"standalone_backend_registers_nothing", standalone_backend_registers_nothing},
        {"attachment_registers_kernel_notification_once",
         attachment_registers_kernel_notification_once},
        {"forced_registration_failure_fails_context_construction",
         forced_registration_failure_fails_context_construction},
        {"teardown_unregisters_before_ring_exit", teardown_unregisters_before_ring_exit},
    };

    std::size_t passed = 0;
    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
        ++passed;
        std::printf("ok %s\n", t.name);
        std::fflush(stdout);
    }
    std::printf("all %zu uring registration lifecycle tests passed\n", passed);
    return 0;
}
