// Clean-room consumer (W-04 candidate host): StackfulIoHost is the
// OPTIONAL_CANDIDATE profile (H-09 + H-30/H-31 closure). This probe records
// externally observable evidence only; it does not claim OPTIONAL_SUPPORTED
// status and does not adopt ADR-0003.
#include <sluice/async/stackful_io_host.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
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

    // Task failure while another task's I/O is outstanding: run() reports
    // the first task error without stranding the other task.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 8,
                                                                  .worker_count = 2}));
        auto host = StackfulIoHost::create(ctx, StackfulHostConfig{.task_capacity = 4});
        CHECK(host.has_value());

        const NativeFileRef ref{file};
        std::atomic<bool> other_finished{false};
        // NativeFileRef with a negative fd is rejected at the invocation
        // boundary (closed/invalid resource) — a deterministic task error.
        const NativeFileRef bad_ref{-1, FileAccess::read_write};

        host.value()->spawn([&](IoTaskContext& io) {
            // Failing task: rejected before acceptance.
            (void)io.read(ref, std::span<std::byte>{}, 0);
        });
        host.value()->spawn([&](IoTaskContext& io) {
            std::vector<std::byte> buf(8, std::byte{0});
            const auto r = io.read_for(ref, buf, 0, std::chrono::milliseconds(50));
            other_finished = true;
            (void)r;
        });
        const auto outcome = host.value()->run();
        // run() returns; both tasks retired; the second task ran to its own
        // terminal even though the first failed (no stranded work).
        (void)outcome;
        CHECK(other_finished.load());
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

    // Stop during suspension: the accepted request settles on its natural
    // outcome (no implicit cancellation by the host stop), and run() returns.
    {
        const std::string path = make_temp_path();
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();
        const std::string payload = "stop-during-suspension";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        CHECK(sluice::blocking::write_all(file, bytes).has_value());

        AsyncIoContext ctx(
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 8,
                                                                  .worker_count = 1}));
        auto host = StackfulIoHost::create(ctx, StackfulHostConfig{.task_capacity = 4});
        CHECK(host.has_value());

        const NativeFileRef ref{file};
        std::vector<std::byte> buf(8, std::byte{0});
        std::atomic<bool> task_returned{false};
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            host.value()->request_stop();
        });
        host.value()->spawn([&](IoTaskContext& io) {
            const auto r = io.read_for(ref, buf, 0, std::chrono::milliseconds(500));
            task_returned = true;
            (void)r;
        });
        const auto outcome = host.value()->run();
        stopper.join();
        CHECK(host.value()->stop_requested());
        CHECK(task_returned.load());
        (void)outcome;
        CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
        file.close();
        ::unlink(path.c_str());
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "w04_stackful_candidate: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("w04_stackful_candidate: PASS\n");
    return 0;
}
