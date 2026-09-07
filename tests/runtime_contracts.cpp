#include <sluice/async/application_runtime.hpp>
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/await_op_helpers.hpp>
#include <sluice/async/cancel.hpp>
#include <sluice/async/group.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include "test_harness.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

using sluice::Result;
using sluice::async::AsyncBackend;
using sluice::async::ApplicationRuntime;
using sluice::async::AsyncIoContext;
using sluice::async::CancelToken;
using sluice::async::Completion;
using sluice::async::Group;
using sluice::async::ReadOp;
using sluice::async::RuntimeBuilder;
using sluice::async::RuntimeTaskContext;
using sluice::async::Scheduler;
using sluice::async::TaskResultSlot;
using sluice::async::ThreadPoolBackend;
using sluice::async::WaitNode;
using sluice::async::WaitQueue;
using sluice::async::translate_task_exception;

std::string runtime_pattern_bytes(std::size_t count) {
    std::string bytes(count, '\0');
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<char>((i * 17 + 3) & 0xFF);
    }
    return bytes;
}

int open_file_read_only(const std::string& path) { return ::open(path.c_str(), O_RDONLY); }

SLUICE_TEST(application_runtime_executes_read_task_to_result) {
    const std::string content = runtime_pattern_bytes(8192);
    sluice_test::TempFile file(content);
    const int fd = open_file_read_only(file.path());
    SLUICE_CHECK(fd >= 0);

    RuntimeBuilder builder;
    builder.backend(std::make_unique<ThreadPoolBackend>());
    builder.workers(2);
    auto build_result = builder.build();
    SLUICE_CHECK(build_result.has_value());
    std::unique_ptr<ApplicationRuntime> runtime = std::move(build_result.value());

    auto started = runtime->start();
    SLUICE_CHECK(started.has_value());

    TaskResultSlot<Result<std::vector<std::byte>>> result_slot;
    Completion<std::size_t> completion;
    auto submitted = runtime->submit(
        [&result_slot, &completion, fd, size = content.size()](RuntimeTaskContext& context) {
            std::vector<std::byte> buffer(size);
            auto read_result =
                sluice::async::await_read_fill(context, fd, buffer, 0, completion);
            if (!read_result.has_value()) {
                result_slot.publish(
                    sluice::make_unexpected<std::vector<std::byte>>(read_result.error()));
                return;
            }
            result_slot.publish(Result<std::vector<std::byte>>(std::move(buffer)));
        });
    SLUICE_CHECK(submitted.has_value());

    Result<std::vector<std::byte>> task_result = result_slot.wait_and_take();
    SLUICE_CHECK(task_result.has_value());
    SLUICE_CHECK(task_result.value().size() == content.size());
    SLUICE_CHECK(std::memcmp(task_result.value().data(), content.data(), content.size()) == 0);

    runtime->request_stop();
    auto drained = runtime->drain();
    SLUICE_CHECK(drained.has_value());
    auto joined = runtime->join();
    SLUICE_CHECK(joined.has_value());

    ::close(fd);
}

SLUICE_TEST(task_result_slot_publishes_once_and_transfers_first_value) {
    TaskResultSlot<int> slot;
    slot.publish(41);
    slot.publish(99);
    const int taken = slot.wait_and_take();
    SLUICE_CHECK(taken == 41);
}

SLUICE_TEST(translate_task_exception_maps_bad_alloc_to_no_space) {
    Result<int> translated = Result<int>(0);
    try {
        throw std::bad_alloc();
    } catch (...) {
        translated = translate_task_exception<int>();
    }
    SLUICE_CHECK(!translated.has_value());
    SLUICE_CHECK(translated.error().code == sluice::IoError::Code::no_space);
}

SLUICE_TEST(cancel_token_request_rearm_clear_epoch_cycle) {
    CancelToken token;
    SLUICE_CHECK(!token.is_requested());
    SLUICE_CHECK(token.epoch() == 0);

    token.request();
    SLUICE_CHECK(token.is_requested());
    SLUICE_CHECK(token.epoch() == 1);

    token.rearm();
    SLUICE_CHECK(token.is_requested());
    SLUICE_CHECK(token.epoch() == 2);

    token.clear();
    SLUICE_CHECK(!token.is_requested());
    SLUICE_CHECK(token.epoch() == 2);

    token.request();
    SLUICE_CHECK(token.is_requested());
    SLUICE_CHECK(token.epoch() == 3);
}

class TestBackendWithoutWaitSupport final : public AsyncBackend {
  public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override {
        return {};
    }
    Result<void> submit_write(sluice::async::WriteOp, Completion<std::size_t>&) override {
        return {};
    }
    Result<void> submit_sync_data(sluice::async::SyncDataOp, Completion<void>&) override {
        return {};
    }
    Result<void> submit_sync_all(sluice::async::SyncAllOp, Completion<void>&) override {
        return {};
    }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return static_cast<std::size_t>(0); }
    std::size_t outstanding() const noexcept override { return 0; }
};

SLUICE_TEST(runtime_builder_rejects_backend_without_wait_support) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<TestBackendWithoutWaitSupport>());
    builder.workers(1);
    auto build_result = builder.build();
    SLUICE_CHECK(!build_result.has_value());
    SLUICE_CHECK(build_result.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_TEST(scheduler_deadline_expires_unwaited_wait) {
    AsyncIoContext io_context(std::make_unique<ThreadPoolBackend>());
    Scheduler scheduler(io_context);
    Group group(scheduler);

    std::atomic<bool> wait_returned{false};
    std::atomic<Scheduler::deadline_t> observed_elapsed{0};

    group.async([&](CancelToken&) {
        WaitQueue wait_queue;
        WaitNode wait_node;
        const Scheduler::deadline_t started_at = scheduler.monotonic_now();
        scheduler.await_wait_deadline(wait_queue, wait_node, started_at + 25);
        observed_elapsed = scheduler.monotonic_now() - started_at;
        wait_returned = true;
    });

    struct DeadlineStopWatch {
        std::atomic<bool>& wait_returned;
    };
    DeadlineStopWatch stop_watch{wait_returned};
    scheduler.run_live(
        1,
        [](void* context) -> bool {
            return static_cast<DeadlineStopWatch*>(context)->wait_returned.load();
        },
        &stop_watch);

    SLUICE_CHECK(wait_returned.load());
    SLUICE_CHECK(observed_elapsed.load() >= 24);
}

} // namespace

SLUICE_TEST_MAIN()
