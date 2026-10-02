#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/file.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <csignal>
#include <sys/wait.h>

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;

// Negative structural evidence: the backend-facing capability exposes no
// waiting, acknowledgement, arming, or owner-control operation, the backend no
// longer hosts a wait-source seam, and the backend wait surface is gone — the
// context refuses to wait rather than delegating.
template <class Port>
concept port_has_no_wait_api =
    !requires(Port p, detail::ProgressSource::Token t) {
        p.wait_if_unchanged(t);
        p.snapshot();
        p.interrupt();
        p.acknowledge_notification();
        p.notification_fd();
    };
static_assert(port_has_no_wait_api<detail::BackendProgressPort>);

template <class B>
concept backend_has_no_wait_source_member = !requires { &B::wait_source; };
static_assert(backend_has_no_wait_source_member<AsyncBackend>);

template <class B>
concept backend_has_no_wait_member =
    !requires(B b) { b.wait_one(); } && !requires { &B::wait_one_is_nonblocking; };
static_assert(backend_has_no_wait_member<AsyncBackend>);

// The C2-C convergence removed the poll-only readiness-fd lend: neither the
// source nor the backend port exposes a readiness-fd binding operation.
template <class S>
concept progress_source_has_no_readiness_lend =
    !requires(S s) { s.bind_physical_readiness(0); };
static_assert(progress_source_has_no_readiness_lend<detail::ProgressSource>);

template <class Port>
concept port_has_no_readiness_lend = !requires(Port p) { p.bind_physical_readiness(0); };
static_assert(port_has_no_readiness_lend<detail::BackendProgressPort>);

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

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_progress_source_XXXXXX";
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

bool context_owns_one_progress_source() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));

    if (ctx.progress_notification_fd() < 0)
        return false;
    const auto token = ctx.progress_token_for_test();
    if (token.progress != 0)
        return false;

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    if (!ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), FileAccess::read_only), buffer.data(), 0, 0}, c)
             .has_value())
        return false;

    // The zero-op publication is a physical progress signal routed through the
    // backend capability into the context notification fd.
    const auto after = ctx.progress_token_for_test();
    if (after.progress <= token.progress)
        return false;
    if (!notification_fd_readable(ctx.progress_notification_fd()))
        return false;

    // Stale readiness is acknowledged only through the context operation.
    ctx.acknowledge_progress_notification();
    if (notification_fd_readable(ctx.progress_notification_fd()))
        return false;

    const auto one = ctx.wait_one(std::chrono::milliseconds{2000});
    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
    if (!one.has_value() || one.value().kind != WaitKind::progress || one.value().completed != 1)
        return false;
    const bool ready = c.ready();
    c.reset();
    ctx.detach_progress_host();
    return ready;
}

bool non_progress_backend_stays_out_of_the_wait_protocol() {
    class NullBackend final : public AsyncBackend {
      public:
        std::size_t poll() override { return 0; }
        std::size_t outstanding() const noexcept override { return 0; }
        std::size_t slot_capacity() const noexcept override { return 0; }
        detail::PublicCancel cancel_identity(detail::RequestKey) override {
            return detail::PublicCancel::not_found;
        }
        Result<detail::RequestKey> submit_read(ReadOp, Completion<std::size_t>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_write(WriteOp, Completion<std::size_t>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_sync_data(SyncDataOp, Completion<void>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_sync_all(SyncAllOp, Completion<void>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
    };

    AsyncIoContext ctx(std::make_unique<NullBackend>());
    if (ctx.progress_notification_fd() < 0)
        return false;
    if (ctx.has_split_wait_capability())
        return false;
    if (ctx.has_bounded_split_wait_capability())
        return false;
    const auto r = ctx.wait_one(std::chrono::milliseconds{1});
    const bool refused = !r.has_value() && r.error().code == IoError::Code::not_supported;
    ctx.detach_progress_host();
    return refused;
}

bool thread_pool_context_declares_physical_progress() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    return ctx.has_split_wait_capability() && ctx.has_bounded_split_wait_capability();
}

bool move_construction_preserves_progress_responsibility() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    const std::string path = make_temp_file("progress move source");
    if (path.empty())
        return false;
    auto file = File::open(path, FileOpen{FileAccess::read_only});
    if (!file.has_value())
        return false;

    std::vector<std::byte> buffer(16, std::byte{0});
    Completion<std::size_t> c;
    if (!ctx.submit_read(ReadOp{NativeFileRef(file.value()), buffer.data(), buffer.size(), 0}, c)
             .has_value())
        return false;

    AsyncIoContext moved(std::move(ctx));

    std::atomic<int> prepark{0};
    moved.set_progress_prepark_counter_for_test(&prepark);

    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
    bool woke = false;
    std::size_t driven = 0;
    {
        std::thread driver([&] {
            auto r = moved.wait_one(std::chrono::milliseconds{5000});
            if (r.has_value() && r.value().kind == WaitKind::progress) {
                woke = true;
                driven = r.value().completed;
            }
        });
        for (int i = 0; i < 500000 && prepark.load(std::memory_order_acquire) < 1; ++i)
            std::this_thread::yield();
        if (prepark.load(std::memory_order_acquire) < 1) {
            driver.join();
            moved.set_progress_prepark_counter_for_test(nullptr);
            raw->set_worker_claimed_pause_gate(nullptr);
            gate.resume.store(true, std::memory_order_release);
            gate.resume.notify_all();
            c.reset();
            return false;
        }
        raw->set_worker_claimed_pause_gate(nullptr);
        gate.resume.store(true, std::memory_order_release);
        gate.resume.notify_all();
        driver.join();
    }
    moved.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = woke && driven == 1 && c.ready() &&
                    notification_fd_readable(moved.progress_notification_fd());
    c.reset();
    moved.detach_progress_host();
    return ok;
}

bool move_assignment_preserves_progress_responsibility() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    if (!ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), FileAccess::read_only), buffer.data(), 0, 0}, c)
             .has_value())
        return false;

    AsyncIoContext target(std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1}));
    const int previous_fd = target.progress_notification_fd();
    target.detach_progress_host();
    target = std::move(ctx);

    if (ctx.progress_notification_fd() != -1)
        return false;
    const auto inert_wait = ctx.wait_one(std::chrono::milliseconds{1});
    if (inert_wait.has_value() || inert_wait.error().code != IoError::Code::invalid_state)
        return false;

    // The moved-to context owns the submitted responsibility and the source
    // notification source; its own source is gone with the move.
    if (!notification_fd_readable(target.progress_notification_fd()))
        return false;
    if (target.progress_notification_fd() == previous_fd)
        return false;
    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
    const auto one = target.wait_one(std::chrono::milliseconds{2000});
    if (!one.has_value() || one.value().kind != WaitKind::progress || one.value().completed != 1)
        return false;
    const bool ready = c.ready();
    c.reset();
    target.detach_progress_host();
    return ready;
}

bool moved_from_context_is_inert() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    AsyncIoContext moved(std::move(ctx));

    const auto wait = ctx.wait_one();
    if (wait.has_value() || wait.error().code != IoError::Code::invalid_state)
        return false;
    if (ctx.progress_notification_fd() != -1)
        return false;
    if (ctx.has_split_wait_capability())
        return false;
    ctx.acknowledge_progress_notification();
    return true;
}

bool backend_destructor_runs_while_progress_source_lives() {
    class DtorSignalBackend final : public AsyncBackend {
      public:
        explicit DtorSignalBackend(std::atomic<bool>* flag) noexcept : flag_(flag) {}

        ~DtorSignalBackend() override {
            progress_port_.signal();
            flag_->store(true, std::memory_order_release);
        }

      private:
        std::atomic<bool>* flag_;

        std::size_t poll() override { return 0; }
        std::size_t outstanding() const noexcept override { return 0; }
        std::size_t slot_capacity() const noexcept override { return 0; }
        detail::PublicCancel cancel_identity(detail::RequestKey) override {
            return detail::PublicCancel::not_found;
        }
        Result<detail::RequestKey> submit_read(ReadOp, Completion<std::size_t>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_write(WriteOp, Completion<std::size_t>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_sync_data(SyncDataOp, Completion<void>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_sync_all(SyncAllOp, Completion<void>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
    };

    std::atomic<bool> signaled_in_destruction{false};
    auto backend = std::make_unique<DtorSignalBackend>(&signaled_in_destruction);
    {
        AsyncIoContext ctx(std::move(backend));
        if (ctx.progress_notification_fd() < 0)
            return false;
        ctx.detach_progress_host();
    }
    // The backend retired first; under sanitizers a ProgressSource destroyed
    // before the backend would surface as use-after-free in its signal.
    return signaled_in_destruction.load(std::memory_order_acquire);
}

bool constructor_unwind_destroys_backend_before_progress_source() {
    class UnwindSignalBackend final : public AsyncBackend {
      public:
        explicit UnwindSignalBackend(std::atomic<bool>* signaled) noexcept
            : signaled_(signaled) {}

        ~UnwindSignalBackend() override {
            progress_port_.signal();
            signaled_->store(true, std::memory_order_release);
        }

        std::size_t slot_capacity() const noexcept override {
            // A capacity the core storage can never satisfy: RequestCore
            // construction throws length_error before any allocation, after
            // the port is already attached.
            return std::numeric_limits<std::size_t>::max();
        }

      private:
        std::size_t poll() override { return 0; }
        std::size_t outstanding() const noexcept override { return 0; }
        detail::PublicCancel cancel_identity(detail::RequestKey) override {
            return detail::PublicCancel::not_found;
        }
        Result<detail::RequestKey> submit_read(ReadOp, Completion<std::size_t>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_write(WriteOp, Completion<std::size_t>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_sync_data(SyncDataOp, Completion<void>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_sync_all(SyncAllOp, Completion<void>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }

        std::atomic<bool>* signaled_;
    };

    std::atomic<bool> signaled{false};
    try {
        AsyncIoContext ctx(std::make_unique<UnwindSignalBackend>(&signaled));
        (void)ctx;
        return false;
    } catch (const std::length_error&) {
        // Under sanitizers, unwinding in the wrong member order aborts inside
        // the backend destructor's port signal on the freed source.
        return signaled.load(std::memory_order_acquire);
    } catch (const std::bad_alloc&) {
        return signaled.load(std::memory_order_acquire);
    }
}

bool unattached_backend_control_ops_are_absent() {
    auto threadpool = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    threadpool->close_admission();
#if defined(SLUICE_HAS_LIBURING)
    auto uring = std::make_unique<UringAsyncBackend>();
    uring->close_admission();
#endif
    return true;
}

template <class Backend>
bool close_admission_signal_tracks_the_admission_transition(Backend* raw,
                                                            AsyncIoContext& ctx) {
    const auto before = ctx.progress_token_for_test();

    raw->close_admission();
    const auto first = ctx.progress_token_for_test();
    if (first.progress != before.progress + 1 ||
        first.progress_exhaustion != before.progress_exhaustion) {
        return false;
    }
    if (!notification_fd_readable(ctx.progress_notification_fd())) {
        return false;
    }
    ctx.acknowledge_progress_notification();
    if (notification_fd_readable(ctx.progress_notification_fd())) {
        return false;
    }

    raw->close_admission();
    raw->close_admission();
    const auto repeated = ctx.progress_token_for_test();
    if (repeated.progress != first.progress ||
        repeated.progress_exhaustion != first.progress_exhaustion) {
        return false;
    }
    if (notification_fd_readable(ctx.progress_notification_fd())) {
        return false;
    }

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    const auto submit =
        ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), FileAccess::read_only),
                               buffer.data(), 0, 0},
                        c);
    return !submit.has_value() && submit.error().code == IoError::Code::invalid_state;
}

bool threadpool_close_admission_signals_only_the_open_to_closed_transition() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    const bool ok = close_admission_signal_tracks_the_admission_transition(raw, ctx);
    ctx.detach_progress_host();
    return ok;
}

#if defined(SLUICE_HAS_LIBURING)
bool uring_available() {
    UringAsyncBackend backend;
    return backend.available();
}

bool uring_close_admission_signals_only_the_open_to_closed_transition() {
    if (!uring_available())
        return true;
    auto backend = std::make_unique<UringAsyncBackend>();
    UringAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    const bool ok = close_admission_signal_tracks_the_admission_transition(raw, ctx);
    ctx.detach_progress_host();
    return ok;
}
#endif

bool uring_zero_op_signals_context_notification() {
#if defined(SLUICE_HAS_LIBURING)
    if (!uring_available())
        return true;

    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    if (!ctx.has_split_wait_capability())
        return false;

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    if (!ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), FileAccess::read_only), buffer.data(), 0, 0}, c)
             .has_value())
        return false;
    if (!notification_fd_readable(ctx.progress_notification_fd()))
        return false;
    ctx.acknowledge_progress_notification();
    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
    const auto one = ctx.wait_one(std::chrono::milliseconds{2000});
    if (!one.has_value() || one.value().kind != WaitKind::progress || one.value().completed != 1)
        return false;
    const bool ready = c.ready();
    c.reset();
    ctx.detach_progress_host();
    return ready;
#else
    return true;
#endif
}

bool uring_completion_wakes_parked_driver() {
#if defined(SLUICE_HAS_LIBURING)
    if (!uring_available())
        return true;

    const std::string path = make_temp_file("progress uring parked wake");
    if (path.empty())
        return false;
    auto file = File::open(path, FileOpen{FileAccess::read_only});
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));

    std::vector<std::byte> buffer(16, std::byte{0});
    Completion<std::size_t> c;
    if (!ctx.submit_read(ReadOp{NativeFileRef(file.value()), buffer.data(), buffer.size(), 0}, c)
             .has_value())
        return false;

    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
    const auto r = ctx.wait_one(std::chrono::milliseconds{5000});
    if (!r.has_value() || r.value().kind != WaitKind::progress || r.value().completed != 1)
        return false;
    const bool ready = c.ready();
    c.reset();
    return ready;
#else
    return true;
#endif
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"context_owns_one_progress_source", context_owns_one_progress_source},
        {"non_progress_backend_stays_out_of_the_wait_protocol",
         non_progress_backend_stays_out_of_the_wait_protocol},
        {"thread_pool_context_declares_physical_progress",
         thread_pool_context_declares_physical_progress},
        {"move_construction_preserves_progress_responsibility",
         move_construction_preserves_progress_responsibility},
        {"move_assignment_preserves_progress_responsibility",
         move_assignment_preserves_progress_responsibility},
        {"moved_from_context_is_inert", moved_from_context_is_inert},
        {"backend_destructor_runs_while_progress_source_lives",
         backend_destructor_runs_while_progress_source_lives},
        {"constructor_unwind_destroys_backend_before_progress_source",
         constructor_unwind_destroys_backend_before_progress_source},
        {"unattached_backend_control_ops_are_absent", unattached_backend_control_ops_are_absent},
        {"threadpool_close_admission_signals_only_the_open_to_closed_transition",
         threadpool_close_admission_signals_only_the_open_to_closed_transition},
#if defined(SLUICE_HAS_LIBURING)
        {"uring_close_admission_signals_only_the_open_to_closed_transition",
         uring_close_admission_signals_only_the_open_to_closed_transition},
#endif
        {"uring_zero_op_signals_context_notification",
         uring_zero_op_signals_context_notification},
        {"uring_completion_wakes_parked_driver", uring_completion_wakes_parked_driver},
    };

    std::size_t passed = 0;
    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
        ++passed;
    }
    std::printf("all %zu progress source ownership tests passed\n", passed);
    return 0;
}
