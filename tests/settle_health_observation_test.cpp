#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/test_access.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/detail/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::FileAccess;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::fflush(stderr);
        ++g_failures;
    }
}

int temp_file_fd(const std::string& content) {
    char path[] = "/tmp/sluice_settle_health_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return -1;
    ::unlink(path);
    if (!content.empty()) {
        const ssize_t n = ::write(fd, content.data(), content.size());
        if (n != static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return -1;
        }
    }
    ::lseek(fd, 0, SEEK_SET);
    return fd;
}

constexpr int kFoldProbesPerPass = 1;
#if !defined(SLUICE_RESEARCH_ELIDE_013)
constexpr int kSettleRequeryProbes = 1;
#endif

// A read pinned inside a paused worker keeps settlement unconverged for as
// long as its gate stays closed, so every settle iteration reaches the
// health observation point; the pinned state can never be destroyed, so the
// scenarios that leave it stuck run in forked witnesses.
struct PinnedRead {
    ThreadPoolBackend::WorkerClaimedPauseGate worker_gate;
    std::vector<std::byte> buffer = std::vector<std::byte>(32, std::byte{0});
    int fd = -1;
    Request<std::size_t> held;

    bool pin(AsyncIoContext& ctx, ThreadPoolBackend* backend) {
        fd = temp_file_fd(std::string(32, 's'));
        if (fd < 0)
            return false;
        backend->set_worker_claimed_pause_gate(&worker_gate);
        auto submitted = ctx.submit_read(
            ReadOp{NativeFileRef(fd, FileAccess::read_only), buffer.data(), buffer.size(), 0});
        if (!submitted.has_value()) {
            backend->set_worker_claimed_pause_gate(nullptr);
            return false;
        }
        held = std::move(submitted).value();
        while (!worker_gate.paused.load(std::memory_order_acquire))
            std::this_thread::yield();
        backend->set_worker_claimed_pause_gate(nullptr);
        return true;
    }

    void release() {
        worker_gate.resume.store(true, std::memory_order_release);
        worker_gate.resume.notify_all();
    }
};

// Two signal-driven park rounds with the read pinned, then the released read
// converges the third pass: three passes, two of them reaching the settle
// health observation.
void counted_two_iteration_settle() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    PinnedRead pinned;
    if (!pinned.pin(ctx, raw)) {
        check(false, "the pinned read stages");
        return;
    }

    std::atomic<int> probe_counter{0};
    std::atomic<int> prepark_counter{0};
    detail::TestAccess::context_set_wait_health_probe_counter(ctx, &probe_counter);
    detail::TestAccess::context_set_progress_prepark_counter(ctx, &prepark_counter);

    std::thread releaser([&] {
        while (prepark_counter.load(std::memory_order_acquire) < 1)
            std::this_thread::yield();
        detail::TestAccess::context_progress_source(ctx)->signal();
        while (prepark_counter.load(std::memory_order_acquire) < 2)
            std::this_thread::yield();
        pinned.release();
    });

    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    releaser.join();

    detail::TestAccess::context_set_wait_health_probe_counter(ctx, nullptr);
    detail::TestAccess::context_set_progress_prepark_counter(ctx, nullptr);

    check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
          "the released settle completes");
    check(ctx.execution_closed(), "the completed settle closes execution");
#if defined(SLUICE_RESEARCH_ELIDE_013)
    constexpr int kExpectedProbes = 3 * kFoldProbesPerPass;
#else
    constexpr int kExpectedProbes = 3 * kFoldProbesPerPass + 2 * kSettleRequeryProbes;
#endif
    check(probe_counter.load() == kExpectedProbes,
          "the wait-health acquisition count matches the settle shape");

    const auto observation = pinned.held.take_result();
    check(observation.readiness == RequestReadiness::ready && observation.result.has_value() &&
              observation.result.value() == 32,
          "the released read completes with its full transfer");
    ::close(pinned.fd);
}

// The sticky verdict observed before settlement must surface as
// health_failed — the outcome only the settle health observation produces —
// with exactly one acquisition per reached observation point.
void sticky_verdict_before_settle_witness() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    PinnedRead pinned;
    if (!pinned.pin(ctx, raw)) {
        check(false, "the pinned read stages");
        return;
    }

    detail::TestAccess::context_set_wait_health_failed(ctx);

    std::atomic<int> probe_counter{0};
    detail::TestAccess::context_set_wait_health_probe_counter(ctx, &probe_counter);

    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    detail::TestAccess::context_set_wait_health_probe_counter(ctx, nullptr);

    check(closed.has_value() && closed.value() == ShutdownOutcome::health_failed,
          "the pre-settle sticky verdict reports health_failed");
    check(!ctx.execution_closed(), "a health_failed settle keeps execution open");
#if defined(SLUICE_RESEARCH_ELIDE_013)
    constexpr int kExpectedProbes = 1 * kFoldProbesPerPass;
#else
    constexpr int kExpectedProbes = 1 * kFoldProbesPerPass + 1 * kSettleRequeryProbes;
#endif
    check(probe_counter.load() == kExpectedProbes,
          "the single settle iteration acquires the wait health exactly per arm");
    std::_Exit(g_failures == 0 ? 0 : 1);
}

// A verdict written inside the observation window — the only write position
// the drive mutex leaves open — must still end settlement as health_failed:
// same-iteration capture where the observation re-reads, and detection at the
// park entry where the pass carried the value.
void window_written_verdict_witness() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    PinnedRead pinned;
    if (!pinned.pin(ctx, raw)) {
        check(false, "the pinned read stages");
        return;
    }

    detail::ProgressSource::PauseGate window_gate;
    detail::TestAccess::context_set_settle_window_pause_gate(ctx, &window_gate);

    std::atomic<int> probe_counter{0};
    detail::TestAccess::context_set_wait_health_probe_counter(ctx, &probe_counter);

    std::thread injector([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!window_gate.paused.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline)
                return;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        detail::TestAccess::context_set_wait_health_failed(ctx);
        window_gate.resume.store(true, std::memory_order_release);
        window_gate.resume.notify_all();
    });

    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    injector.join();

    detail::TestAccess::context_set_wait_health_probe_counter(ctx, nullptr);
    detail::TestAccess::context_set_settle_window_pause_gate(ctx, nullptr);

    check(closed.has_value() && closed.value() == ShutdownOutcome::health_failed,
          "the window-written verdict still reports health_failed");
    check(!ctx.execution_closed(), "a health_failed settle keeps execution open");
#if defined(SLUICE_RESEARCH_ELIDE_013)
    constexpr int kExpectedProbes = 1 * kFoldProbesPerPass;
#else
    constexpr int kExpectedProbes = 1 * kFoldProbesPerPass + 1 * kSettleRequeryProbes;
#endif
    check(probe_counter.load() == kExpectedProbes,
          "the window iteration acquires the wait health exactly per arm");
    std::_Exit(g_failures == 0 ? 0 : 1);
}

// The witnesses never return: the unsettled contexts they hold must stay
// alive, so the exit code is taken from inside while the state stands.
int run_witness(void (*witness)()) {
    const pid_t pid = ::fork();
    if (pid < 0)
        return 3;
    if (pid == 0) {
        ::alarm(30);
        witness();
        std::_Exit(2);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid)
        return 3;
    if (WIFSIGNALED(status))
        return 3;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 3;
}

} // namespace

int main() {
    counted_two_iteration_settle();
    const int sticky_exit = run_witness(sticky_verdict_before_settle_witness);
    const int window_exit = run_witness(window_written_verdict_witness);
    check(sticky_exit == 0, "the pre-settle sticky witness passes");
    check(window_exit == 0, "the window-written verdict witness passes");
    if (g_failures == 0) {
        std::printf("all settle health observation checks passed\n");
        return 0;
    }
    std::printf("%d settle health observation checks failed\n", g_failures);
    return 1;
}
