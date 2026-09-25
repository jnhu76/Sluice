#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_PUBLIC_REQUEST_URING)
#include <sluice/async/uring_backend.hpp>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::File;

#if defined(SLUICE_PUBLIC_REQUEST_URING)

using Backend = UringAsyncBackend;

std::unique_ptr<Backend> make_backend() {
    return std::make_unique<UringAsyncBackend>(UringConfig{2, 8});
}

#else

using Backend = ThreadPoolBackend;

std::unique_ptr<Backend> make_backend() {
    return std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
}

#endif

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_b2_violation_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    if (!content.empty()) {
        const ssize_t n = ::write(fd, content.data(), content.size());
        if (n != static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return {};
        }
    }
    ::close(fd);
    return path;
}

// Each violating scenario runs in its own forked child, freshly constructed
// after the fork, so the parent never holds a violating object and the child's
// always-on diagnostic is the only way the scenario can end.
bool child_dies_running(void (*scenario)()) {
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        ::alarm(30);
        scenario();
        std::_Exit(0);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid)
        return false;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

void destroy_nonterminal_request() {
    auto file_path = make_temp_file("sluice b2 nonterminal destructor\n");
    auto opened = File::open(file_path);
    ::unlink(file_path.c_str());
    if (!opened.has_value())
        std::_Exit(2);
    File file = std::move(opened.value());

    auto backend = make_backend();
    AsyncIoContext ctx(std::move(backend));

    std::vector<std::byte> buffer(8, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), buffer.size(), 0});
    if (!submitted.has_value())
        std::_Exit(2);
    Request<std::size_t> request = std::move(submitted).value();
    // No publication drive: the request is accepted but not published.
    {
        Request<std::size_t> violating = std::move(request);
        (void)violating;
    }
    std::_Exit(0);
}

void move_assign_over_nonterminal_request() {
    auto file_path = make_temp_file("sluice b2 move assign violation\n");
    auto opened = File::open(file_path);
    ::unlink(file_path.c_str());
    if (!opened.has_value())
        std::_Exit(2);
    File file = std::move(opened.value());

    auto pending_backend = make_backend();
    AsyncIoContext pending_ctx(std::move(pending_backend));
    auto ready_backend = make_backend();
    AsyncIoContext ready_ctx(std::move(ready_backend));

    std::vector<std::byte> buffer(8, std::byte{0});
    auto pending_submit =
        pending_ctx.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), buffer.size(), 0});
    auto ready_submit =
        ready_ctx.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), buffer.size(), 0});
    if (!pending_submit.has_value() || !ready_submit.has_value())
        std::_Exit(2);
    Request<std::size_t> pending = std::move(pending_submit).value();
    Request<std::size_t> ready = std::move(ready_submit).value();
    while (!ready.ready())
        (void)ready_ctx.poll();

    Request<std::size_t>& destination = pending;
    destination = std::move(ready);
    std::_Exit(0);
}

void destroy_context_with_live_public_binding() {
    auto file_path = make_temp_file("sluice b2 v21\n");
    auto opened = File::open(file_path);
    ::unlink(file_path.c_str());
    if (!opened.has_value())
        std::_Exit(2);
    File file = std::move(opened.value());

    auto backend = make_backend();
    AsyncIoContext ctx(std::move(backend));

    std::vector<std::byte> buffer(8, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), buffer.size(), 0});
    if (!submitted.has_value())
        std::_Exit(2);
    Request<std::size_t> request = std::move(submitted).value();
    while (!request.ready())
        (void)ctx.poll();
    // The published result is still bound; context destruction is forbidden.
    ctx.~AsyncIoContext();
    std::_Exit(0);
}

template <class Gate> void wait_gate_paused(Gate& gate) noexcept {
    std::atomic<bool>& paused = gate.paused;
    bool seen = paused.load(std::memory_order_acquire);
    while (!seen) {
        paused.wait(seen, std::memory_order_acquire);
        seen = paused.load(std::memory_order_acquire);
    }
}

void discard_request_during_publication_inflight() {
    auto file_path = make_temp_file("sluice b2 inflight discard\n");
    auto opened = File::open(file_path);
    ::unlink(file_path.c_str());
    if (!opened.has_value())
        std::_Exit(2);
    File file = std::move(opened.value());

    auto backend = make_backend();
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Backend::PublicationEpiloguePauseGate gate;
    raw->set_publication_epilogue_pause_gate(&gate);

    std::vector<std::byte> buffer(8, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), buffer.size(), 0});
    if (!submitted.has_value())
        std::_Exit(2);
    Request<std::size_t> request = std::move(submitted).value();

    std::atomic<bool> stop_driver{false};
    std::thread driver([&] {
        while (!stop_driver.load(std::memory_order_acquire))
            (void)ctx.poll();
    });
    wait_gate_paused(gate);
    if (request.ready())
        std::_Exit(3);
    request.discard();
    std::_Exit(0);
}

struct NamedScenario {
    const char* name;
    void (*violation)();
};

}

int main() {
#if defined(SLUICE_PUBLIC_REQUEST_URING)
    {
        auto probe = make_backend();
        if (!probe->available()) {
            std::printf("SKIP release violation tests: io_uring is unavailable\n");
            return 0;
        }
    }
#endif
    const NamedScenario scenarios[] = {
        {"destroy_nonterminal_request", destroy_nonterminal_request},
        {"move_assign_over_nonterminal_request", move_assign_over_nonterminal_request},
        {"destroy_context_with_live_public_binding", destroy_context_with_live_public_binding},
        {"discard_request_during_publication_inflight", discard_request_during_publication_inflight},
    };
    for (const NamedScenario& scenario : scenarios) {
        if (!child_dies_running(scenario.violation)) {
            std::fprintf(stderr, "FAIL [%s] the always-on diagnostic did not terminate\n",
                         scenario.name);
            return 1;
        }
        std::printf("PASS [%s] the always-on diagnostic terminated the process\n",
                    scenario.name);
    }
    std::printf("all %zu public request release violation scenarios passed\n",
                sizeof(scenarios) / sizeof(scenarios[0]));
    return 0;
}
