#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_SHUTDOWN_URING)
#include <sluice/async/uring_backend.hpp>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(SLUICE_SHUTDOWN_URING)
#include <liburing.h>
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::IoError;
using sluice::async::detail::RequestCore;

struct Tracker {
    const char* name;
    int failures = 0;
    bool skipped = false;

    void check(bool ok, const char* label) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s\n", name, label);
        }
    }

    void skip(const char* reason) {
        skipped = true;
        std::printf("SKIP [%s] %s\n", name, reason);
    }
};

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_e2_shutdown_XXXXXX";
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

std::optional<File> open_temp_file(Tracker& t, const std::string& content) {
    const std::string path = make_temp_file(content);
    if (path.empty()) {
        t.check(false, "the temporary fixture file is created");
        return std::nullopt;
    }
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value()) {
        t.check(false, "the temporary fixture file opens");
        return std::nullopt;
    }
    return std::optional<File>{std::move(opened.value())};
}

// Readiness waits are bounded by a wall-clock deadline, not an iteration
// budget: a runnable worker can lose any fixed spin count to CPU contention
// (the release-job flake, #483). The loop keeps the test's own publication
// pass as the observation point, yields cooperatively so a runnable worker
// wins the deadline race under CPU pressure, and reports the observed state
// when the deadline expires instead of silently ending the budget.
template <class Ready>
bool poll_until_ready(AsyncIoContext& ctx, std::chrono::milliseconds budget,
                      const char* what, Tracker& t, Ready&& ready) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    unsigned long long passes = 0;
    bool poll_failed = false;
    while (!ready()) {
        const auto polled = ctx.poll();
        ++passes;
        if (!polled.has_value()) {
            poll_failed = true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr,
                         "TIMEOUT [%s] %s after %llu poll passes (%sfailed poll: %s)\n",
                         t.name, what, passes, poll_failed ? "" : "no ",
                         poll_failed ? "see earlier failures" : "polls healthy");
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

#if defined(SLUICE_SHUTDOWN_URING)

using Backend = UringAsyncBackend;

std::unique_ptr<Backend> make_backend() {
    try {
        return std::make_unique<UringAsyncBackend>(UringConfig{4, 8});
    } catch (...) {
        return nullptr;
    }
}

constexpr std::uint64_t kControlTag = std::uint64_t{1} << 63u;

int fake_submit_no_kernel(void*, ::io_uring* ring) noexcept {
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(ready);
}

struct SequencedSubmitContext {
    int calls = 0;
};

// The sequenced fiction consumes every hand-off except the second submit
// call, which fails hard without submitting: the first operation stays
// kernel-visible (its ledger entry is reconciled away) and keeps its real
// completion path after the poison, while the second converges through
// poison recovery.
int sequenced_submit(void* ctx, ::io_uring* ring) noexcept {
    auto* state = static_cast<SequencedSubmitContext*>(ctx);
    const int call = state->calls++;
    if (call == 1)
        return -EIO;
    return fake_submit_no_kernel(ctx, ring);
}

struct FictionBackend {
    std::unique_ptr<AsyncIoContext> ctx;
    UringAsyncBackend* backend = nullptr;
    RequestCore* core = nullptr;

    static FictionBackend create(std::size_t capacity,
                                 UringBackendSubmitTestHooks::SubmitFn submit_fn = nullptr,
                                 void* submit_ctx = nullptr) {
        UringBackendSubmitTestHooks hooks;
        hooks.submit = submit_fn != nullptr ? submit_fn : fake_submit_no_kernel;
        hooks.context = submit_ctx;
        auto owned = std::make_unique<UringAsyncBackend>(UringConfig{capacity, 8}, hooks);
        FictionBackend made;
        made.backend = owned.get();
        made.ctx = std::make_unique<AsyncIoContext>(std::move(owned));
        made.core = made.ctx->context_core_for_test();
        return made;
    }

    bool ready() const { return ctx != nullptr; }
};

#else

using Backend = ThreadPoolBackend;

std::unique_ptr<Backend> make_backend() {
    return std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
}

#endif  // SLUICE_SHUTDOWN_URING

// Each violating scenario runs in its own forked child, freshly constructed
// after the fork, so the parent never holds a violating object and the
// child's always-on diagnostic is the only way the scenario can end.
bool scenario_dies_aborting(void (*scenario)()) {
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

// The contract names its always-on diagnostics; the death scenario must end
// at the named guard, not at any abort that happens to be nearby.
bool scenario_dies_aborting_named(void (*scenario)(), const char* needle) {
    char diag[] = "/tmp/sluice_e2_death_XXXXXX";
    const int capture = ::mkstemp(diag);
    if (capture < 0)
        return false;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(capture);
        ::unlink(diag);
        return false;
    }
    if (pid == 0) {
        ::dup2(capture, STDERR_FILENO);
        ::close(capture);
        ::alarm(30);
        scenario();
        std::_Exit(0);
    }
    ::close(capture);
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        ::unlink(diag);
        return false;
    }
    std::string captured;
    {
        char buffer[512];
        const int fd = ::open(diag, O_RDONLY);
        if (fd >= 0) {
            ssize_t n;
            while ((n = ::read(fd, buffer, sizeof(buffer))) > 0)
                captured.append(buffer, static_cast<std::size_t>(n));
            ::close(fd);
        }
    }
    ::unlink(diag);
    if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT))
        return false;
    return captured.find(needle) != std::string::npos;
}

void destroy_context_with_pending_live_binding() {
    auto opened = File::open(make_temp_file("shutdown pending binding\n"));
    if (!opened.has_value())
        std::_Exit(2);
    File file = std::move(opened.value());

    auto backend = make_backend();
    if (backend == nullptr)
        std::_Exit(2);
    AsyncIoContext ctx(std::move(backend));

    std::vector<std::byte> buffer(8, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), buffer.size(), 0});
    if (!submitted.has_value())
        std::_Exit(2);
    Request<std::size_t> request = std::move(submitted).value();
    (void)request;
    // Accepted, unpublished, binding live: destruction is a caller violation.
    ctx.~AsyncIoContext();
    std::_Exit(0);
}

void destroy_context_with_live_host_interest() {
    auto backend = make_backend();
    if (backend == nullptr)
        std::_Exit(2);
    AsyncIoContext ctx(std::move(backend));
    auto owner = ctx.claim_progress_owner();
    if (!owner.has_value())
        std::_Exit(2);
    // A live progress owner is an external registration the host still owns.
    ctx.~AsyncIoContext();
    std::_Exit(0);
}

bool shutdown_completes_and_retained_results_stay_consumable(Tracker& t) {
    auto file = open_temp_file(t, "shutdown retained results payload");
    if (!file.has_value())
        return true;

    auto backend = make_backend();
    if (backend == nullptr) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx(std::move(backend));

    std::vector<std::byte> first(16, std::byte{0});
    std::vector<std::byte> second(16, std::byte{0});
    auto a = ctx.submit_read(ReadOp{NativeFileRef{*file}, first.data(), first.size(), 0});
    auto b = ctx.submit_read(ReadOp{NativeFileRef{*file}, second.data(), second.size(), 8});
    if (!a.has_value() || !b.has_value()) {
        t.check(false, "both reads are accepted");
        return true;
    }
    Request<std::size_t> ra = std::move(a).value();
    Request<std::size_t> rb = std::move(b).value();
    (void)poll_until_ready(ctx, std::chrono::milliseconds(10000),
                           "both retained reads become ready", t,
                           [&] { return ra.ready() && rb.ready(); });
    t.check(ra.ready() && rb.ready(), "both results are ready before the shutdown");

    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the drain shutdown reports completion");
    t.check(!ctx.admission_open(), "admission is closed after the shutdown");
    t.check(ctx.execution_closed(), "execution is closed after the shutdown");

    auto again = ctx.shutdown(ShutdownPolicy::cancel_then_drain);
    t.check(again.has_value() && again.value() == ShutdownOutcome::completed,
            "a shutdown of the execution-closed context is idempotent");

    auto observed = ra.try_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 16,
            "the first retained result is still observable after execution close");
    ra.discard();
    auto consumed = rb.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value() &&
                consumed.result.value() == 16,
            "the second retained result is still consumable after execution close");

    std::vector<std::byte> late_buffer(4, std::byte{0});
    auto late = ctx.submit_read(
        ReadOp{NativeFileRef{*file}, late_buffer.data(), late_buffer.size(), 0});
    t.check(!late.has_value(), "a submission against the closed admission is refused");
    return true;
}

#if defined(SLUICE_SHUTDOWN_URING)
// THREAD-01 lets request_stop race the owner's drive, and the upgrade it
// records must reach a driver that already entered its drain. The pipe stays
// open and empty, so the read can never complete on its own and convergence
// is reachable only through the mid-drive cancellation.
bool stop_upgrade_during_parked_shutdown_converges(Tracker& t) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        t.check(false, "the upgrade scenario forks");
        return true;
    }
    if (pid == 0) {
        ::alarm(30);
        auto backend = make_backend();
        if (backend == nullptr)
            std::_Exit(3);
        AsyncIoContext ctx(std::move(backend));

        int fds[2];
        if (::pipe(fds) != 0)
            std::_Exit(3);
        std::vector<std::byte> buffer(16, std::byte{0});
        auto submitted = ctx.submit_read(
            ReadOp{NativeFileRef{fds[0], sluice::FileAccess::read_only}, buffer.data(),
                   buffer.size(), 0});
        if (!submitted.has_value())
            std::_Exit(4);
        Request<std::size_t> request = std::move(submitted).value();

        std::atomic<bool> done{false};
        bool closed_ok = false;
        ShutdownOutcome outcome = ShutdownOutcome::health_failed;
        std::thread driver([&] {
            auto closed = ctx.shutdown(ShutdownPolicy::drain);
            closed_ok = closed.has_value();
            if (closed.has_value())
                outcome = closed.value();
            done.store(true, std::memory_order_release);
        });
        // drive_settlement_ closes admission as its first act, so observing
        // it closed places the racing stop strictly inside the live drive.
        for (int i = 0; i < 2000000 && ctx.admission_open(); ++i) {
        }
        ctx.request_stop(ShutdownPolicy::cancel_then_drain);
        driver.join();

        bool ok = done.load(std::memory_order_acquire) && closed_ok &&
                  outcome == ShutdownOutcome::completed;
        auto observed = request.try_result();
        ok = ok && observed.readiness == RequestReadiness::ready &&
             !observed.result.has_value() &&
             observed.result.error().code == IoError::Code::canceled;
        std::_Exit(ok ? 0 : 1);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        t.check(false, "the upgrade scenario is reaped");
        return true;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 3) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 4) {
        t.skip("the kernel refused the pipe read");
        return true;
    }
    t.check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "the mid-drive stop upgrade converges the shutdown and cancels the read");
    return true;
}
#endif

#if defined(SLUICE_SHUTDOWN_URING)
// A borrow that lands after the shutdown linearized but before the drive
// exits must not observe the notification fd that this same shutdown will
// retire, so the parked drain is the deterministic observation point.
bool shutdown_blocks_new_notification_interest_after_drive_linearization(Tracker& t) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        t.check(false, "the borrow-linearization scenario forks");
        return true;
    }
    if (pid == 0) {
        ::alarm(30);
        auto backend = make_backend();
        if (backend == nullptr)
            std::_Exit(3);
        int code = 1;
        {
            AsyncIoContext ctx(std::move(backend));

            int fds[2];
            if (::pipe(fds) != 0)
                std::_Exit(3);
            std::vector<std::byte> buffer(16, std::byte{0});
            auto submitted = ctx.submit_read(
                ReadOp{NativeFileRef{fds[0], sluice::FileAccess::read_only}, buffer.data(),
                       buffer.size(), 0});
            if (!submitted.has_value())
                std::_Exit(4);
            Request<std::size_t> request = std::move(submitted).value();

            const int setup_fd = ctx.progress_notification_fd();
            if (setup_fd < 0)
                std::_Exit(5);
            ctx.detach_progress_host();

            std::atomic<bool> done{false};
            bool closed_ok = false;
            ShutdownOutcome outcome = ShutdownOutcome::health_failed;
            std::thread driver([&] {
                auto closed = ctx.shutdown(ShutdownPolicy::drain);
                closed_ok = closed.has_value();
                if (closed.has_value())
                    outcome = closed.value();
                done.store(true, std::memory_order_release);
            });
            for (int i = 0; i < 2000000 && ctx.admission_open(); ++i) {
            }
            const int racing = ctx.progress_notification_fd();
            ctx.request_stop(ShutdownPolicy::cancel_then_drain);
            driver.join();

            bool ok = done.load(std::memory_order_acquire) && closed_ok &&
                      outcome == ShutdownOutcome::completed;
            ok = ok && racing == -1;
            ok = ok && ctx.progress_notification_fd() == -1;
            auto observed = request.try_result();
            ok = ok && observed.readiness == RequestReadiness::ready &&
                 !observed.result.has_value() &&
                 observed.result.error().code == IoError::Code::canceled;
            code = ok ? 0 : 1;
            request.discard();
        }
        std::_Exit(code);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        t.check(false, "the borrow-linearization scenario is reaped");
        return true;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 3) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 4) {
        t.skip("the kernel refused the pipe read");
        return true;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 5) {
        t.skip("the backend exposes no notification fd");
        return true;
    }
    t.check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "the shutdown drive lends no new notification interest");
    return true;
}
#endif

#if defined(SLUICE_SHUTDOWN_URING)
bool cancel_then_drain_shutdown_cancels_pending_without_fabricated_success(Tracker& t) {
    auto file = open_temp_file(t, "shutdown cancel pending payload");
    if (!file.has_value())
        return true;

    FictionBackend made = FictionBackend::create(4);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;

    std::vector<std::byte> buffer(32, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    if (!submitted.has_value()) {
        t.check(false, "the pending read is accepted");
        return true;
    }
    Request<std::size_t> request = std::move(submitted).value();
    t.check(!request.ready(), "the read is pending before any drive");

    ctx.request_stop(ShutdownPolicy::cancel_then_drain);

    // The pending read is already kernel-visible, so the recorded policy
    // issues its cancel control and the kernel race resolves on the real
    // completion. The fiction delivers both CQEs while the driver parks.
    std::atomic<bool> fire{false};
    std::thread kernel([&] {
        while (!fire.load(std::memory_order_acquire)) {
        }
        // The control must be submitted before its CQE exists; a settlement
        // that skipped the passes never submits it and the injection would
        // hit a control the ring never sent.
        for (int i = 0; i < 4000000 && made.backend->live_control_sqes_for_test() == 0; ++i)
            std::this_thread::yield();
        if (made.backend->live_control_sqes_for_test() == 0)
            return;
        const auto cookie = made.backend->live_cookie_for_offset_for_test(0);
        if (cookie.has_value()) {
            made.backend->inject_cqe_for_test(*cookie, 32);
            made.backend->inject_cqe_for_test(kControlTag | *cookie, 0);
        }
        ctx.interrupt_progress_waiters();
    });
    fire.store(true, std::memory_order_release);

    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the cancel-then-drain policy converges to completion");
    kernel.join();
    t.check(ctx.execution_closed(), "execution is closed by the shutdown");
    t.check(made.backend->live_control_sqes_for_test() == 0,
            "the cancel control retired with the settlement");

    auto observed = request.try_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 32,
            "the pending request settles on its real kernel outcome, not a fabricated "
            "cancel terminal");
    t.check(request.take_result().readiness == RequestReadiness::ready,
            "the terminal stays consumable after execution close");
    auto again = ctx.shutdown(ShutdownPolicy::cancel_then_drain);
    t.check(again.has_value() && again.value() == ShutdownOutcome::completed,
            "a shutdown of the execution-closed context is idempotent");
    return true;
}

#endif

bool destruction_with_pending_live_binding_fails_fast(Tracker& t) {
    t.check(scenario_dies_aborting_named(destroy_context_with_pending_live_binding,
                                         "outstanding public bindings"),
            "destroying a context under a pending live binding aborts at the "
            "named public-binding guard");
    return true;
}

bool destruction_with_live_host_interest_fails_fast(Tracker& t) {
    t.check(scenario_dies_aborting(destroy_context_with_live_host_interest),
            "destroying a context with a live progress owner aborts");
    return true;
}

bool shutdown_retires_the_notification_fd_and_reuse_is_inert(Tracker& t) {
    auto backend = make_backend();
    if (backend == nullptr) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx(std::move(backend));

    const int notification_fd = ctx.progress_notification_fd();
    t.check(notification_fd >= 0, "the context exposes a progress notification fd");
    t.check(!ctx.shutdown(ShutdownPolicy::drain).has_value(),
            "a shutdown under the live host interest is refused");

    ctx.detach_progress_host();
    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the shutdown completes");
    t.check(ctx.progress_notification_fd() == -1,
            "the context no longer exposes the retired notification fd");
    ctx.detach_progress_host();
    errno = 0;
    t.check(::fcntl(notification_fd, F_GETFD) == -1 && errno == EBADF,
            "the notification descriptor is really closed");

    // Reuse the exact descriptor number: any stale wake from the retired
    // source would now land on this live eventfd.
    const int reused = ::eventfd(0, EFD_NONBLOCK);
    if (reused < 0) {
        t.check(false, "a fresh eventfd is created for the reuse probe");
        return true;
    }
    if (reused != notification_fd) {
        if (::dup2(reused, notification_fd) < 0) {
            t.check(false, "the fresh eventfd is placed on the retired number");
            ::close(reused);
            return true;
        }
        ::close(reused);
    }

    ctx.close_admission();
    ctx.request_stop(ShutdownPolicy::cancel_then_drain);

    struct ::pollfd pfd { notification_fd, POLLIN, 0 };
    t.check(::poll(&pfd, 1, 0) >= 0 && pfd.revents == 0,
            "no stale wake reaches the reused descriptor");
    std::uint64_t drained = 0;
    const ssize_t got = ::read(notification_fd, &drained, sizeof(drained));
    t.check(got <= 0, "the reused descriptor carries no spurious notification");

    ::close(notification_fd);
    return true;
}

bool observer_delivery_episode_settles_at_shutdown(Tracker& t) {
    auto file = open_temp_file(t, "shutdown observer episode payload");
    if (!file.has_value())
        return true;

    auto backend = make_backend();
    if (backend == nullptr) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(16, std::byte{0});
    Completion<std::size_t> completion;
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0},
                        completion);
    if (!submitted.has_value()) {
        t.check(false, "the observed read is accepted");
        return true;
    }
    auto attached = ctx.attach_observer(completion);
    t.check(attached.armed(), "the observer arms before publication");

    (void)poll_until_ready(ctx, std::chrono::milliseconds(10000), "the observed read publishes",
                           t, [&] { return completion.ready(); });
    t.check(completion.ready(), "the observed read publishes");

    const auto episode = core.observe_slot(detail::SlotIndex{0});
    t.check(episode.has_value() && episode->observer_phase == detail::ObserverPhase::delivering,
            "the delivery episode is claimed and stays deferred without a consumer");

    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the shutdown settles the deferred delivery episode");
    const auto settled = core.observe_slot(detail::SlotIndex{0});
    t.check(settled.has_value() && settled->observer_phase == detail::ObserverPhase::retired,
            "the delivery episode is retired after the shutdown");
    t.check(completion.result().has_value(), "the observed result stays intact");
    return true;
}

bool observer_attach_after_admission_close_is_refused(Tracker& t) {
    auto file = open_temp_file(t, "shutdown observer admission payload");
    if (!file.has_value())
        return true;

    auto backend = make_backend();
    if (backend == nullptr) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx(std::move(backend));

    std::vector<std::byte> buffer(16, std::byte{0});
    Completion<std::size_t> early;
    Completion<std::size_t> late;
    auto first =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0}, early);
    auto second = ctx.submit_read(
        ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 4}, late);
    if (!first.has_value() || !second.has_value()) {
        t.check(false, "both observed candidates are accepted");
        return true;
    }
    t.check(ctx.attach_observer(early).armed(), "the early observer arms");

    ctx.close_admission();
    t.check(!ctx.admission_open(), "admission is closed");

    const auto refused = ctx.attach_observer(late);
    t.check(refused.status == detail::ObserverRegistration::admission_closed,
            "an observer attach after the admission close is refused");

    (void)poll_until_ready(ctx, std::chrono::milliseconds(10000),
                           "both observed candidates become ready", t,
                           [&] { return early.ready() && late.ready(); });
    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the shutdown converges with the refused attach");
    return true;
}

#if defined(SLUICE_SHUTDOWN_URING)
bool close_wins_the_reserve_window(Tracker& t) {
    auto file = open_temp_file(t, "shutdown reserve window payload");
    if (!file.has_value())
        return true;

    FictionBackend made = FictionBackend::create(4);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;

    Backend::SubmitEntryPauseGate gate;
    made.backend->set_submit_entry_pause_gate(&gate);

    std::vector<std::byte> buffer(16, std::byte{0});
    std::atomic<bool> done{false};
    std::optional<sluice::Result<void>> outcome;
    Completion<std::size_t> completion;
    std::thread racer([&] {
        outcome =
            ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0},
                            completion);
        done.store(true, std::memory_order_release);
    });

    while (!gate.paused.load(std::memory_order_acquire))
        std::this_thread::yield();
    // The racer parks inside the context submit path, so the racing close
    // must act on the core's admission authority directly.
    core.close_admission();
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
    racer.join();

    t.check(done.load() && outcome.has_value() && !outcome->has_value(),
            "the reservation refused after the admission close loses");
    const auto snap = core.snapshot();
    t.check(snap.reserved == 0 && snap.accepted_live == 0,
            "the losing submission leaves no slot residue");

    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the shutdown converges after the lost race");
    return true;
}

bool accepted_before_close_stays_in_the_finite_set(Tracker& t) {
    auto file = open_temp_file(t, "shutdown commit window payload");
    if (!file.has_value())
        return true;

    FictionBackend made = FictionBackend::create(4);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;

    Backend::AcceptedPreDispatchPauseGate gate;
    made.backend->set_accepted_pre_dispatch_pause_gate(&gate);

    std::vector<std::byte> buffer(16, std::byte{0});
    std::optional<sluice::Result<Request<std::size_t>>> outcome;
    std::thread racer([&] {
        outcome =
            ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    });

    while (!gate.paused.load(std::memory_order_acquire))
        std::this_thread::yield();
    // The commit already happened: the racing close acts on the core's
    // admission authority directly, and the accepted request belongs to the
    // finite accepted set.
    made.core->close_admission();
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
    racer.join();

    t.check(outcome.has_value() && outcome->has_value(),
            "the committed reservation wins and is accepted");

    Request<std::size_t> request = std::move(outcome->value());
    t.check(ctx.poll().has_value(), "the accepted request still reaches the kernel");
    const auto cookie = made.backend->live_cookie_for_offset_for_test(0);
    t.check(cookie.has_value(), "the accepted request holds a live transport cookie");
    made.backend->inject_cqe_for_test(*cookie, 16);
    (void)poll_until_ready(ctx, std::chrono::milliseconds(10000),
                           "the accepted request settles despite the closed admission", t,
                           [&] { return request.ready(); });
    t.check(request.ready(), "the accepted request settles despite the closed admission");
    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the shutdown converges with the accepted request settled");
    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value() &&
                consumed.result.value() == 16,
            "the winning result stays consumable");
    return true;
}
#endif

bool shutdown_refuses_while_host_interest_is_live(Tracker& t) {
    auto backend = make_backend();
    if (backend == nullptr) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx(std::move(backend));

    // A live notification registration refuses the shutdown outright.
    const int fd = ctx.progress_notification_fd();
    t.check(fd >= 0, "the host registers its notification interest");
    auto refused = ctx.shutdown(ShutdownPolicy::drain);
    t.check(!refused.has_value() && refused.error().code == IoError::Code::invalid_state,
            "a shutdown under live host notification interest is refused");
    ctx.detach_progress_host();

    // A live progress owner keeps the drive domain: the shutdown is refused
    // from a non-owner thread and admissible from the owner thread.
    auto owner = ctx.claim_progress_owner();
    t.check(owner.has_value(), "the host claims the progress owner");
    std::atomic<bool> foreign_refused{false};
    std::thread foreign([&] {
        foreign_refused.store(!ctx.shutdown(ShutdownPolicy::drain).has_value(),
                              std::memory_order_release);
    });
    foreign.join();
    t.check(foreign_refused.load(), "a non-owner thread's shutdown is refused");
    auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the owner thread's shutdown completes");
    return true;
}

bool shutdown_from_non_owner_after_execution_close_is_refused(Tracker& t) {
    auto backend = make_backend();
    if (backend == nullptr) {
        t.skip("the backend is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx(std::move(backend));

    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the owner's shutdown completes on the idle context");
    t.check(ctx.execution_closed(), "the execution-closed record stands");

    const auto owner_repeat = ctx.shutdown(ShutdownPolicy::drain);
    t.check(owner_repeat.has_value() && owner_repeat.value() == ShutdownOutcome::completed,
            "the owner's repeated shutdown returns the recorded outcome");

    std::atomic<bool> foreign_invalid{false};
    std::thread foreign([&] {
        const auto refused = ctx.shutdown(ShutdownPolicy::drain);
        foreign_invalid.store(!refused.has_value() &&
                                  refused.error().code == IoError::Code::invalid_state,
                              std::memory_order_release);
    });
    foreign.join();
    t.check(foreign_invalid.load(),
            "a non-owner's shutdown after execution close is refused as invalid state");
    return true;
}

#if !defined(SLUICE_SHUTDOWN_URING)
bool zero_op_internal_pin_retires_at_shutdown(Tracker& t) {
    auto file = open_temp_file(t, "shutdown zero-op pin payload");
    if (!file.has_value())
        return true;

    auto backend = make_backend();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    auto submitted = ctx.submit_read(ReadOp{NativeFileRef{*file}, nullptr, 0, 0});
    if (!submitted.has_value()) {
        t.check(false, "the zero-op read is accepted");
        return true;
    }
    Request<std::size_t> request = std::move(submitted).value();
    t.check(request.ready(), "the zero-op publishes inline");
    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready, "the zero-op result is consumed");

    const auto witness = core.observe_slot(detail::SlotIndex{0});
    t.check(witness.has_value() && witness->published && !witness->binding_live &&
                witness->control_refs == 1,
            "the consumed zero-op leaves exactly its internal control pin");

    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the settlement retires the internal pin and completes");
    return true;
}
#endif

#if defined(SLUICE_SHUTDOWN_URING)
bool dispatch_failure_poison_retires_through_real_failure_terminals(Tracker& t) {
    auto file = open_temp_file(t, "shutdown poison dispatch payload");
    if (!file.has_value())
        return true;

    FictionBackend made = FictionBackend::create(4);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;

    UringAsyncBackend::DispatchFailureInjection injection;
    made.backend->set_dispatch_failure_injection(&injection);
    injection.armed.store(true);

    std::vector<std::byte> buffer(16, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(submitted.has_value(), "the poisoned dispatch still accepts its request");
    injection.armed.store(false);
    t.check(injection.fired.load() == 1, "the dispatch failure fired exactly once");

    Request<std::size_t> request = std::move(submitted).value();
    (void)poll_until_ready(ctx, std::chrono::milliseconds(10000),
                           "the poisoned request converges through its failure terminal", t,
                           [&] { return request.ready(); });
    auto observed = request.try_result();
    t.check(observed.readiness == RequestReadiness::ready && !observed.result.has_value(),
            "the poison converges through a real failure terminal");
    t.check(observed.result.error().code == IoError::Code::backend_error,
            "the failure terminal carries the backend health error");

    t.check(request.take_result().readiness == RequestReadiness::ready,
            "the failure terminal is consumable");
    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the fully retired poison is a safe-retirement completion");
    t.check(ctx.execution_closed(), "the safe retirement closes execution");
    return true;
}

// The scenario leaves the context deliberately unsettled: the in-flight
// request keeps its borrow forever (its completion never arrives), so
// destruction over this state is a contract violation. The parent leaks the
// state on purpose; only the forked death scenario may destroy it.
struct PoisonedState {
    std::unique_ptr<AsyncIoContext> ctx;
    UringAsyncBackend* backend = nullptr;
    Request<std::size_t> in_flight;
    Request<std::size_t> poisoned;
};

std::unique_ptr<PoisonedState> poison_with_in_flight_cqe(Tracker& t) {
    auto file = open_temp_file(t, "shutdown poison in-flight payload");
    if (!file.has_value())
        return nullptr;

    SequencedSubmitContext sequenced{};
    FictionBackend made = FictionBackend::create(4, sequenced_submit, &sequenced);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return nullptr;
    }
    UringAsyncBackend* backend = made.backend;

    std::vector<std::byte> first(16, std::byte{0});
    std::vector<std::byte> second(16, std::byte{0});
    auto a =
        made.ctx->submit_read(ReadOp{NativeFileRef{*file}, first.data(), first.size(), 0});
    if (!a.has_value()) {
        t.check(false, "the first request is accepted");
        return nullptr;
    }
    t.check(made.ctx->poll().has_value(), "the first request reaches the fiction kernel");
    auto b = made.ctx->submit_read(
        ReadOp{NativeFileRef{*file}, second.data(), second.size(), 0});
    if (!b.has_value()) {
        t.check(false, "the second request is accepted before the failure");
        return nullptr;
    }
    t.check(made.ctx->poll().has_value(), "the second acceptance's transport poisons");

    auto state = std::make_unique<PoisonedState>();
    state->ctx = std::move(made.ctx);
    state->backend = backend;
    state->in_flight = std::move(a).value();
    state->poisoned = std::move(b).value();

    (void)poll_until_ready(*state->ctx, std::chrono::milliseconds(10000),
                           "the never-visible request converges through the poison terminal",
                           t, [&] { return state->poisoned.ready(); });
    auto observed = state->poisoned.try_result();
    t.check(observed.readiness == RequestReadiness::ready && !observed.result.has_value(),
            "the never-visible request converges through the poison terminal");
    t.check(state->backend->live_cookie_for_offset_for_test(0).has_value(),
            "the kernel-visible request keeps its real completion path");
    t.check(!state->in_flight.ready(),
            "the in-flight request carries no fabricated terminal");
    return state;
}

bool poison_with_in_flight_cqe_reports_unresolved_health(Tracker& t) {
    auto state = poison_with_in_flight_cqe(t);
    if (state == nullptr)
        return true;

    const auto closed = state->ctx->shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::health_unresolved,
            "the stuck in-flight borrow reports unresolved health");
    t.check(!state->ctx->execution_closed(),
            "an unresolved health outcome keeps execution open");
    t.check(state->poisoned.take_result().readiness == RequestReadiness::ready,
            "the poison terminal stays consumable in the unsettled context");
    // Deliberately leaked: destruction of this state is a contract violation.
    (void)state.release();
    return true;
}

void destroy_poisoned_context() {
    Tracker child{"poison destruction death"};
    auto state = poison_with_in_flight_cqe(child);
    if (state == nullptr)
        std::_Exit(2);
    const auto closed = state->ctx->shutdown(ShutdownPolicy::drain);
    if (!closed.has_value() || closed.value() != ShutdownOutcome::health_unresolved)
        std::_Exit(3);
    // Unresolved health plus a live in-flight borrow: destruction must abort.
    state->ctx.reset();
    std::_Exit(0);
}

bool destruction_of_poisoned_context_fails_fast(Tracker& t) {
    t.check(scenario_dies_aborting(destroy_poisoned_context),
            "destroying an unsettled poisoned context aborts");
    return true;
}

bool consumed_binding_control_pin_retires_without_new_io(Tracker& t) {
    auto file = open_temp_file(t, "shutdown control pin payload");
    if (!file.has_value())
        return true;

    FictionBackend made = FictionBackend::create(4);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;

    std::vector<std::byte> buffer(32, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    if (!submitted.has_value()) {
        t.check(false, "the canceled read is accepted");
        return true;
    }
    Request<std::size_t> request = std::move(submitted).value();
    const auto disposition = request.cancel();
    t.check(disposition.has_value() && disposition.value() == CancelDisposition::requested,
            "the cancel prepares its control");
    t.check(ctx.poll().has_value(), "the dispatch and control hand-offs happen");

    const auto cookie = made.backend->live_cookie_for_offset_for_test(0);
    t.check(cookie.has_value(), "the original operation has a live cookie");
    made.backend->inject_cqe_for_test(*cookie, 32);
    (void)poll_until_ready(ctx, std::chrono::milliseconds(10000),
                           "the original outcome wins and is consumed", t,
                           [&] { return request.ready(); });
    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value() &&
                consumed.result.value() == 32,
            "the original outcome wins and is consumed");

    const auto witness = core.observe_slot(detail::SlotIndex{0});
    t.check(witness.has_value() && witness->published && !witness->binding_live &&
                witness->control_refs == 1,
            "the consumed binding leaves exactly the cancel control pin");

    const std::uint64_t flushes_before = made.backend->submit_flushes_for_test();

    std::atomic<bool> fire{false};
    std::thread control_delivery([&] {
        while (!fire.load(std::memory_order_acquire)) {
        }
        made.backend->inject_cqe_for_test(kControlTag | *cookie, 0);
        ctx.interrupt_progress_waiters();
    });
    fire.store(true, std::memory_order_release);

    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the settlement retires the control pin and completes");
    control_delivery.join();

    t.check(made.backend->live_control_sqes_for_test() == 0,
            "the cancel control is retired");
    t.check(made.backend->submit_flushes_for_test() == flushes_before,
            "the retirement drove no new kernel submissions");
    return true;
}
#endif  // SLUICE_SHUTDOWN_URING

#if !defined(SLUICE_SHUTDOWN_URING)
// Negative control for the bounded readiness waits: a completion that is
// deliberately withheld (the worker parks on its claim before the syscall)
// must fail by the deadline, never pass on a lucky schedule, and the same
// wait must accept the completion once it is legitimately released.
bool withheld_completion_fails_by_deadline_and_late_completion_passes(Tracker& t) {
    auto file = open_temp_file(t, "shutdown bounded-readiness control payload");
    if (!file.has_value())
        return true;

    auto backend = make_backend();
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Backend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    std::vector<std::byte> buffer(16, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(submitted.has_value(), "the control read is accepted");
    if (!submitted.has_value()) {
        raw->set_worker_claimed_pause_gate(nullptr);
        return true;
    }
    Request<std::size_t> request = std::move(submitted).value();

    while (!gate.paused.load(std::memory_order_acquire))
        std::this_thread::yield();

    const auto began = std::chrono::steady_clock::now();
    const bool observed =
        poll_until_ready(ctx, std::chrono::milliseconds(1500), "the withheld read stays pending",
                         t, [&] { return request.ready(); });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began);
    t.check(!observed && !request.ready(),
            "a withheld completion fails by the bounded deadline");
    t.check(elapsed >= std::chrono::milliseconds(1400),
            "the failed wait consumed its deadline rather than a spin budget");

    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
    t.check(poll_until_ready(ctx, std::chrono::milliseconds(10000),
                             "the released read converges", t, [&] { return request.ready(); }),
            "a delayed but valid completion passes the same bounded wait");
    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value() &&
                consumed.result.value() == 16,
            "the delayed completion carries its real outcome");

    raw->set_worker_claimed_pause_gate(nullptr);
    const auto closed = ctx.shutdown(ShutdownPolicy::drain);
    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the control context shuts down cleanly");
    return true;
}
#endif  // !SLUICE_SHUTDOWN_URING

struct NamedTest {
    const char* name;
    bool (*run)(Tracker&);
};

}  // namespace

int main() {
#if defined(SLUICE_SHUTDOWN_URING)
    {
        auto probe = make_backend();
        if (probe == nullptr || !probe->available()) {
            std::printf("SKIP all shutdown lifecycle tests: io_uring is unavailable\n");
            return 0;
        }
    }
#endif
    const NamedTest tests[] = {
        {"shutdown_completes_and_retained_results_stay_consumable",
         shutdown_completes_and_retained_results_stay_consumable},
#if defined(SLUICE_SHUTDOWN_URING)
        {"cancel_then_drain_shutdown_cancels_pending_without_fabricated_success",
         cancel_then_drain_shutdown_cancels_pending_without_fabricated_success},
        {"stop_upgrade_during_parked_shutdown_converges",
         stop_upgrade_during_parked_shutdown_converges},
        {"shutdown_blocks_new_notification_interest_after_drive_linearization",
         shutdown_blocks_new_notification_interest_after_drive_linearization},
#endif
        {"destruction_with_pending_live_binding_fails_fast",
         destruction_with_pending_live_binding_fails_fast},
        {"destruction_with_live_host_interest_fails_fast",
         destruction_with_live_host_interest_fails_fast},
        {"shutdown_retires_the_notification_fd_and_reuse_is_inert",
         shutdown_retires_the_notification_fd_and_reuse_is_inert},
        {"observer_delivery_episode_settles_at_shutdown",
         observer_delivery_episode_settles_at_shutdown},
        {"observer_attach_after_admission_close_is_refused",
         observer_attach_after_admission_close_is_refused},
#if defined(SLUICE_SHUTDOWN_URING)
        {"close_wins_the_reserve_window", close_wins_the_reserve_window},
        {"accepted_before_close_stays_in_the_finite_set",
         accepted_before_close_stays_in_the_finite_set},
#endif
        {"shutdown_refuses_while_host_interest_is_live",
         shutdown_refuses_while_host_interest_is_live},
        {"shutdown_from_non_owner_after_execution_close_is_refused",
         shutdown_from_non_owner_after_execution_close_is_refused},
#if !defined(SLUICE_SHUTDOWN_URING)
        {"zero_op_internal_pin_retires_at_shutdown",
         zero_op_internal_pin_retires_at_shutdown},
        {"withheld_completion_fails_by_deadline_and_late_completion_passes",
         withheld_completion_fails_by_deadline_and_late_completion_passes},
#else
        {"dispatch_failure_poison_retires_through_real_failure_terminals",
         dispatch_failure_poison_retires_through_real_failure_terminals},
        {"poison_with_in_flight_cqe_reports_unresolved_health",
         poison_with_in_flight_cqe_reports_unresolved_health},
        {"destruction_of_poisoned_context_fails_fast",
         destruction_of_poisoned_context_fails_fast},
        {"consumed_binding_control_pin_retires_without_new_io",
         consumed_binding_control_pin_retires_without_new_io},
#endif
    };

    int failures = 0;
    for (const NamedTest& test : tests) {
        Tracker tracker{test.name};
        test.run(tracker);
        if (!tracker.skipped && tracker.failures == 0)
            std::printf("PASS [%s]\n", test.name);
        failures += tracker.failures;
    }
    if (failures != 0)
        std::fprintf(stderr, "%d shutdown lifecycle check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
