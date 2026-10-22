// BASE-arm companion of the wedged-shutdown probe: identical scenario without
// the E1-only pending-control test seams, so it compiles against the BASE
// async sources. Prints converged/request_terminal and exits without joining
// the wedged thread.

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <liburing.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace {

using namespace sluice::async;

int wedged_submit(void*, ::io_uring* ring) noexcept {
    return 0;
}

} // namespace

int main() {
    ::alarm(120);
    FILE* backing = ::tmpfile();
    if (backing == nullptr)
        return 2;
    std::byte content[4096];
    std::memset(content, 0x33, sizeof(content));
    if (::fwrite(content, 1, sizeof(content), backing) != sizeof(content))
        return 2;
    ::rewind(backing);
    UringBackendSubmitTestHooks hooks;
    hooks.submit = wedged_submit;
    auto owned = std::make_unique<UringAsyncBackend>(UringConfig{4, 1}, hooks);
    auto ctx = std::make_unique<AsyncIoContext>(detail::make_context(std::move(owned)));

    std::byte a[4];
    ReadOp op{NativeFileRef(::fileno(backing), sluice::FileAccess::read_only), a, sizeof(a), 0};
    auto first = ctx->submit_read(op);
    if (!first.has_value())
        return 2;

    std::atomic<bool> done{false};
    std::thread shutdowner([&] {
        (void)ctx->shutdown(ShutdownPolicy::cancel_then_drain);
        done.store(true, std::memory_order_release);
    });
    bool converged = false;
    for (int i = 0; i < 100 && !converged; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        converged = done.load(std::memory_order_acquire);
    }
    std::fprintf(stderr, "probe base_wedged: converged=%d request_terminal=%d\n",
                 converged ? 1 : 0, first.value().ready() ? 1 : 0);
    std::fflush(stderr);
    std::_Exit(0);
}
