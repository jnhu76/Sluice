// Clean-room consumer (W-04 candidate host): StackfulIoHost is the
// OPTIONAL_CANDIDATE profile (H-09 + H-30/H-31 closure). This probe records
// externally observable evidence only; it does not claim OPTIONAL_SUPPORTED
// status and does not adopt ADR-0003.
//
// The suspension-shaped arms (failure while sibling I/O is outstanding, stop
// during suspension) need a backend whose reads can park on a pipe; the
// threadpool backend preads and non-seekable fds fail immediately, so they
// run only under SLUICE_HAS_LIBURING and the binary reports the explicit
// unavailable outcome (exit 2) without it — the same pattern as the w03
// uring variant.
#include <sluice/async/stackful_io_host.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#if defined(SLUICE_HAS_LIBURING)
#include <sluice/async/uring_backend.hpp>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
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
    char path[] = "/tmp/sluice_f1_w04_XXXXXX";
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

#if defined(SLUICE_HAS_LIBURING)

void write_byte(int fd) {
    const char b = 'x';
    (void)::write(fd, &b, 1);
}

void read_byte(int fd) {
    char b;
    (void)::read(fd, &b, 1);
}

#endif

} // namespace

int main() {
    using namespace sluice;
    using namespace sluice::async;

    // Sequential-looking admitted request operations through the host
    // (the W-04 tracer shape: read-fill -> write -> sync), with open/close
    // performed outside the task region on the host thread.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();
        const std::string payload = "stackful-w04";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        CHECK(sluice::blocking::write_all(file, bytes).has_value());

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 8,
                                                                  .worker_count = 2}));
        auto host = StackfulIoHost::create(ctx, StackfulHostConfig{.task_capacity = 4});
        CHECK(host.has_value());

        const NativeFileRef ref{file};
        std::vector<std::byte> buf(payload.size(), std::byte{0});
        const std::string out = "W04-OUT!";
        bool task_ok = false;

        auto run_result = [&]() -> Result<void> {
            // Sequential read -> write -> sync through the task context.
            task_ok = false;
            host.value()->spawn([&](IoTaskContext& io) {
                const auto read = io.read_exact(ref, buf, 0);
                if (!read.has_value())
                    return;
                const auto wrote = io.write_all(ref,
                                                std::span<const std::byte>(
                                                    reinterpret_cast<const std::byte*>(out.data()),
                                                    out.size()),
                                                0);
                if (!wrote.has_value())
                    return;
                const auto synced = io.sync_data(ref);
                task_ok = synced.has_value();
            });
            return host.value()->run();
        }();
        CHECK(run_result.has_value());
        CHECK(task_ok);
        CHECK(std::string(reinterpret_cast<const char*>(buf.data()), buf.size()) == payload);
        // The host write is observable through an independent direct read.
        {
            auto verify = File::open(path, [] {
                sluice::FileOpen mode;
                mode.access = sluice::FileAccess::read_only;
                return mode;
            }());
            CHECK(verify.has_value());
            std::vector<char> content(out.size(), '\0');
            const auto got = sluice::blocking::read_exact_at(
                verify.value(), 0,
                std::span<std::byte>(reinterpret_cast<std::byte*>(content.data()),
                                     content.size()));
            CHECK(got.has_value() && got.value().complete());
            CHECK(std::string(content.data(), content.size()) == out);
            CHECK(verify.value().close().has_value());
        }
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    // Exception during task processing: the host boundary catches it, no
    // borrow is abandoned, and run() returns the mapped error.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();
        const std::string payload = "exception-path";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        CHECK(sluice::blocking::write_all(file, bytes).has_value());

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 8,
                                                                  .worker_count = 1}));
        auto host = StackfulIoHost::create(ctx, StackfulHostConfig{.task_capacity = 4});
        CHECK(host.has_value());

        const NativeFileRef ref{file};
        std::vector<std::byte> buf(4, std::byte{0});
        std::atomic<bool> reached_after_await{false};
        host.value()->spawn([&](IoTaskContext& io) {
            (void)io.read_exact(ref, buf, 0);
            reached_after_await = true;
            throw std::runtime_error("w04 exception probe");
        });
        const auto outcome = host.value()->run();
        CHECK(!outcome.has_value());
        CHECK(outcome.error().code == IoError::Code::backend_error);
        CHECK(reached_after_await.load());
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

#if defined(SLUICE_HAS_LIBURING)

    // A task failure while another task's I/O is genuinely outstanding,
    // structurally ordered: the sibling parks on an empty-pipe read, the
    // single-driver run() starts the failing task only after that
    // suspension, the failing task's throw is announced on throw_pipe, and
    // the writer closes (the sibling's real EOF) only after the throw. The
    // failure therefore lands strictly between the sibling's suspension and
    // its settlement; run() reports the task error and the sibling still
    // settles with its real terminal — no abandoned borrow.
    {
        int data_pipe[2];
        int suspend_pipe[2];
        int throw_pipe[2];
        CHECK(::pipe(data_pipe) == 0);
        CHECK(::pipe(suspend_pipe) == 0);
        CHECK(::pipe(throw_pipe) == 0);

        auto backend = std::make_unique<UringAsyncBackend>(
            UringConfig{.request_capacity = 8, .queue_depth = 8});
        AsyncIoContext ctx(std::move(backend));
        auto host = StackfulIoHost::create(ctx, StackfulHostConfig{.task_capacity = 4});
        CHECK(host.has_value());

        const NativeFileRef reader{data_pipe[0], FileAccess::read_only};
        std::atomic<bool> sibling_returned{false};
        std::atomic<bool> sibling_eof{false};
        std::atomic<bool> sibling_canceled{false};
        host.value()->spawn([&](IoTaskContext& io) {
            std::vector<std::byte> buf(4, std::byte{0});
            const auto r = io.read(reader, buf, 0);
            sibling_eof = r.has_value() && r.value() == 0;
            sibling_canceled = !r.has_value() && r.error().code == IoError::Code::canceled;
            sibling_returned = true;
        });
        host.value()->spawn([&](IoTaskContext& io) {
            (void)io;
            write_byte(suspend_pipe[1]);
            write_byte(throw_pipe[1]);
            throw std::runtime_error("failing task while sibling I/O outstanding");
        });

        std::thread closer([&] {
            read_byte(suspend_pipe[0]);
            read_byte(throw_pipe[0]);
            ::close(data_pipe[1]);
        });
        const auto outcome = host.value()->run();
        closer.join();
        CHECK(!outcome.has_value());
        CHECK(outcome.error().code == IoError::Code::backend_error);
        CHECK(sibling_returned.load());
        CHECK(sibling_eof.load());
        CHECK(!sibling_canceled.load());
        ::close(data_pipe[0]);
        ::close(suspend_pipe[0]);
        ::close(suspend_pipe[1]);
        ::close(throw_pipe[0]);
        ::close(throw_pipe[1]);
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
    }

    // Stop during suspension, structurally ordered: the reader task parks on
    // an empty pipe (writer open), the single-driver run() starts the second
    // task only after that suspension, so the second task's byte on
    // suspend_pipe proves the read request was accepted and outstanding.
    // request_stop is issued only after that byte, and the writer closes
    // (the real EOF) only after request_stop returned. The asserted terminal
    // therefore cannot be reached before the stop request existed, and stop
    // must neither cancel nor settle the accepted request: the task observes
    // the real EOF, the stop flag visible through its token, and retires
    // normally.
    {
        int data_pipe[2];
        int suspend_pipe[2];
        int go_pipe[2];
        CHECK(::pipe(data_pipe) == 0);
        CHECK(::pipe(suspend_pipe) == 0);
        CHECK(::pipe(go_pipe) == 0);

        auto backend = std::make_unique<UringAsyncBackend>(
            UringConfig{.request_capacity = 8, .queue_depth = 8});
        AsyncIoContext ctx(std::move(backend));
        auto host = StackfulIoHost::create(ctx, StackfulHostConfig{.task_capacity = 4});
        CHECK(host.has_value());

        const NativeFileRef reader{data_pipe[0], FileAccess::read_only};
        std::atomic<bool> task_returned{false};
        std::atomic<bool> terminal_eof{false};
        std::atomic<bool> terminal_canceled{false};
        std::atomic<bool> stop_visible_in_task{false};
        host.value()->spawn([&](IoTaskContext& io) {
            std::vector<std::byte> buf(4, std::byte{0});
            const auto r = io.read(reader, buf, 0);
            terminal_eof = r.has_value() && r.value() == 0;
            terminal_canceled = !r.has_value() && r.error().code == IoError::Code::canceled;
            stop_visible_in_task = io.token().is_requested();
            task_returned = true;
        });
        host.value()->spawn([&](IoTaskContext& io) {
            (void)io;
            write_byte(suspend_pipe[1]);
        });

        std::thread stopper([&] {
            read_byte(suspend_pipe[0]);
            host.value()->request_stop();
            write_byte(go_pipe[1]);
        });
        std::thread writer([&] {
            read_byte(go_pipe[0]);
            ::close(data_pipe[1]);
        });
        const auto outcome = host.value()->run();
        stopper.join();
        writer.join();
        CHECK(outcome.has_value());
        CHECK(host.value()->stop_requested());
        CHECK(task_returned.load());
        CHECK(terminal_eof.load());
        CHECK(!terminal_canceled.load());
        CHECK(stop_visible_in_task.load());
        ::close(data_pipe[0]);
        ::close(suspend_pipe[0]);
        ::close(suspend_pipe[1]);
        ::close(go_pipe[0]);
        ::close(go_pipe[1]);
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
    }

    // The bounded (`_for`) path under the same structurally ordered stop: the
    // wait parameter bounds only the driver's park window, so whichever comes
    // first — park-window expiry or the writer-gated EOF — the task must see
    // the operation's real terminal, never a cancellation. The assertions
    // hold under both interleavings, so no timing assumption is baked in.
    {
        int data_pipe[2];
        int suspend_pipe[2];
        int go_pipe[2];
        CHECK(::pipe(data_pipe) == 0);
        CHECK(::pipe(suspend_pipe) == 0);
        CHECK(::pipe(go_pipe) == 0);

        auto backend = std::make_unique<UringAsyncBackend>(
            UringConfig{.request_capacity = 8, .queue_depth = 8});
        AsyncIoContext ctx(std::move(backend));
        auto host = StackfulIoHost::create(ctx, StackfulHostConfig{.task_capacity = 4});
        CHECK(host.has_value());

        const NativeFileRef reader{data_pipe[0], FileAccess::read_only};
        std::atomic<bool> task_returned{false};
        std::atomic<bool> terminal_eof{false};
        std::atomic<bool> terminal_canceled{false};
        std::atomic<bool> stop_visible_in_task{false};
        host.value()->spawn([&](IoTaskContext& io) {
            std::vector<std::byte> buf(4, std::byte{0});
            const auto r = io.read_for(reader, buf, 0, std::chrono::milliseconds(50));
            terminal_eof = r.has_value() && r.value() == 0;
            terminal_canceled = !r.has_value() && r.error().code == IoError::Code::canceled;
            stop_visible_in_task = io.token().is_requested();
            task_returned = true;
        });
        host.value()->spawn([&](IoTaskContext& io) {
            (void)io;
            write_byte(suspend_pipe[1]);
        });

        std::thread stopper([&] {
            read_byte(suspend_pipe[0]);
            host.value()->request_stop();
            write_byte(go_pipe[1]);
        });
        std::thread writer([&] {
            read_byte(go_pipe[0]);
            ::close(data_pipe[1]);
        });
        const auto outcome = host.value()->run();
        stopper.join();
        writer.join();
        CHECK(outcome.has_value());
        CHECK(host.value()->stop_requested());
        CHECK(task_returned.load());
        CHECK(terminal_eof.load());
        CHECK(!terminal_canceled.load());
        CHECK(stop_visible_in_task.load());
        ::close(data_pipe[0]);
        ::close(suspend_pipe[0]);
        ::close(suspend_pipe[1]);
        ::close(go_pipe[0]);
        ::close(go_pipe[1]);
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "w04_stackful_candidate: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("w04_stackful_candidate: PASS\n");
    return 0;

#else

    if (g_failures != 0) {
        std::fprintf(stderr, "w04_stackful_candidate: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("w04_stackful_candidate: structural suspension arms UNAVAILABLE "
                "(threadpool backend preads; non-seekable fds fail immediately)\n");
    return 2;

#endif
}
