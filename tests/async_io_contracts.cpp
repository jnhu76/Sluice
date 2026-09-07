#include <sluice/async/async_io_context.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include "test_harness.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using sluice::async::AsyncIoContext;
using sluice::async::Completion;
using sluice::async::ReadOp;
using sluice::async::SyncAllOp;
using sluice::async::SyncDataOp;
using sluice::async::ThreadPoolBackend;
using sluice::async::WriteOp;
using sluice::Result;

std::string pattern_bytes(std::size_t count) {
    std::string bytes(count, '\0');
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    return bytes;
}

int open_file_read_only(const std::string& path) { return ::open(path.c_str(), O_RDONLY); }

int open_file_read_write(const std::string& path) { return ::open(path.c_str(), O_RDWR); }

Result<std::size_t> drive_completion_to_ready(AsyncIoContext& context,
                                              Completion<std::size_t>& completion) {
    while (!completion.ready()) {
        auto waited = context.wait_one();
        if (!waited.has_value()) {
            return sluice::make_unexpected<std::size_t>(waited.error());
        }
    }
    return completion.result();
}

Result<void> drive_completion_to_ready(AsyncIoContext& context, Completion<void>& completion) {
    while (!completion.ready()) {
        auto waited = context.wait_one();
        if (!waited.has_value()) {
            return sluice::make_unexpected<void>(waited.error());
        }
    }
    return completion.result();
}

SLUICE_TEST(async_read_completes_with_file_bytes) {
    const std::string content = pattern_bytes(4096);
    sluice_test::TempFile file(content);

    AsyncIoContext context(std::make_unique<ThreadPoolBackend>());
    const int fd = open_file_read_only(file.path());
    SLUICE_CHECK(fd >= 0);

    std::vector<std::byte> buffer(content.size());
    Completion<std::size_t> completion;
    auto submitted = context.submit_read(
        ReadOp{fd, buffer.data(), buffer.size(), 0}, completion);
    SLUICE_CHECK(submitted.has_value());

    auto result = drive_completion_to_ready(context, completion);
    SLUICE_CHECK(result.has_value());
    SLUICE_CHECK(result.value() == content.size());
    SLUICE_CHECK(std::memcmp(buffer.data(), content.data(), content.size()) == 0);

    completion.reset();
    SLUICE_CHECK(completion.idle());
    ::close(fd);
}

SLUICE_TEST(async_read_at_eof_completes_with_zero_bytes) {
    const std::string content = pattern_bytes(128);
    sluice_test::TempFile file(content);

    AsyncIoContext context(std::make_unique<ThreadPoolBackend>());
    const int fd = open_file_read_only(file.path());

    std::array<std::byte, 16> buffer{};
    Completion<std::size_t> completion;
    auto submitted = context.submit_read(
        ReadOp{fd, buffer.data(), buffer.size(), content.size()}, completion);
    SLUICE_CHECK(submitted.has_value());

    auto result = drive_completion_to_ready(context, completion);
    SLUICE_CHECK(result.has_value());
    SLUICE_CHECK(result.value() == 0);

    completion.reset();
    ::close(fd);
}

SLUICE_TEST(async_write_stores_bytes_at_offset) {
    const std::string existing = pattern_bytes(256);
    sluice_test::TempFile file(existing);
    const std::string payload = pattern_bytes(64);

    AsyncIoContext context(std::make_unique<ThreadPoolBackend>());
    const int fd = open_file_read_write(file.path());

    Completion<std::size_t> completion;
    auto submitted = context.submit_write(
        WriteOp{fd, reinterpret_cast<const std::byte*>(payload.data()), payload.size(), 128},
        completion);
    SLUICE_CHECK(submitted.has_value());

    auto result = drive_completion_to_ready(context, completion);
    SLUICE_CHECK(result.has_value());
    SLUICE_CHECK(result.value() == payload.size());

    std::string verified(existing.size(), '\0');
    const ssize_t got =
        ::pread(fd, verified.data(), verified.size(), 0);
    SLUICE_CHECK(got == static_cast<ssize_t>(existing.size()));
    SLUICE_CHECK(verified.compare(128, payload.size(), payload) == 0);
    SLUICE_CHECK(verified.compare(0, 128, existing, 0, 128) == 0);

    completion.reset();
    ::close(fd);
}

SLUICE_TEST(async_sync_operations_complete_on_real_file) {
    const std::string content = pattern_bytes(64);
    sluice_test::TempFile file(content);

    AsyncIoContext context(std::make_unique<ThreadPoolBackend>());
    const int fd = open_file_read_write(file.path());

    Completion<void> sync_data_completion;
    auto data_submitted =
        context.submit_sync_data(SyncDataOp{fd}, sync_data_completion);
    SLUICE_CHECK(data_submitted.has_value());
    auto data_result = drive_completion_to_ready(context, sync_data_completion);
    SLUICE_CHECK(data_result.has_value());
    sync_data_completion.reset();

    Completion<void> sync_all_completion;
    auto all_submitted = context.submit_sync_all(SyncAllOp{fd}, sync_all_completion);
    SLUICE_CHECK(all_submitted.has_value());
    auto all_result = drive_completion_to_ready(context, sync_all_completion);
    SLUICE_CHECK(all_result.has_value());
    sync_all_completion.reset();

    ::close(fd);
}

SLUICE_TEST(completion_reset_allows_reuse_for_second_submit) {
    const std::string first_content = pattern_bytes(512);
    const std::string second_content = pattern_bytes(256);
    sluice_test::TempFile file(first_content + second_content);

    AsyncIoContext context(std::make_unique<ThreadPoolBackend>());
    const int fd = open_file_read_only(file.path());

    Completion<std::size_t> completion;
    std::vector<std::byte> buffer(first_content.size());

    auto first_submitted = context.submit_read(
        ReadOp{fd, buffer.data(), buffer.size(), 0}, completion);
    SLUICE_CHECK(first_submitted.has_value());
    auto first_result = drive_completion_to_ready(context, completion);
    SLUICE_CHECK(first_result.has_value());
    SLUICE_CHECK(first_result.value() == first_content.size());
    SLUICE_CHECK(std::memcmp(buffer.data(), first_content.data(), first_content.size()) == 0);

    completion.reset();
    SLUICE_CHECK(completion.idle());

    buffer.assign(second_content.size(), std::byte{0});
    auto second_submitted = context.submit_read(
        ReadOp{fd, buffer.data(), buffer.size(), first_content.size()}, completion);
    SLUICE_CHECK(second_submitted.has_value());
    auto second_result = drive_completion_to_ready(context, completion);
    SLUICE_CHECK(second_result.has_value());
    SLUICE_CHECK(second_result.value() == second_content.size());
    SLUICE_CHECK(std::memcmp(buffer.data(), second_content.data(), second_content.size()) == 0);

    completion.reset();
    ::close(fd);
}

SLUICE_TEST(threadpool_backend_reports_split_wait_capabilities) {
    AsyncIoContext context(std::make_unique<ThreadPoolBackend>());
    SLUICE_CHECK(context.has_split_wait_capability());
    SLUICE_CHECK(context.has_bounded_split_wait_capability());
}

SLUICE_TEST(backend_outstanding_returns_to_zero_after_completion) {
    const std::string content = pattern_bytes(64);
    sluice_test::TempFile file(content);

    AsyncIoContext context(std::make_unique<ThreadPoolBackend>());
    const int fd = open_file_read_only(file.path());

    SLUICE_CHECK(context.outstanding() == 0);

    std::vector<std::byte> buffer(content.size());
    Completion<std::size_t> completion;
    auto submitted = context.submit_read(
        ReadOp{fd, buffer.data(), buffer.size(), 0}, completion);
    SLUICE_CHECK(submitted.has_value());

    auto result = drive_completion_to_ready(context, completion);
    SLUICE_CHECK(result.has_value());
    SLUICE_CHECK(context.outstanding() == 0);

    completion.reset();
    ::close(fd);
}

SLUICE_TEST(backend_close_admission_rejects_new_submissions) {
    const std::string content = pattern_bytes(64);
    sluice_test::TempFile file(content);

    auto backend = std::make_unique<ThreadPoolBackend>();
    ThreadPoolBackend* backend_ptr = backend.get();
    AsyncIoContext context(std::move(backend));
    const int fd = open_file_read_only(file.path());

    std::vector<std::byte> buffer(content.size());
    Completion<std::size_t> completion;
    auto submitted = context.submit_read(
        ReadOp{fd, buffer.data(), buffer.size(), 0}, completion);
    SLUICE_CHECK(submitted.has_value());
    auto result = drive_completion_to_ready(context, completion);
    SLUICE_CHECK(result.has_value());
    completion.reset();

    backend_ptr->close_admission();

    auto rejected = context.submit_read(
        ReadOp{fd, buffer.data(), buffer.size(), 0}, completion);
    SLUICE_CHECK(!rejected.has_value());
    SLUICE_CHECK(rejected.error().code == sluice::IoError::Code::invalid_state);
    SLUICE_CHECK(completion.idle());

    ::close(fd);
}

class HoldingTestBackend final : public sluice::async::AsyncBackend {
  public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>& completion) override {
        return hold_size(completion);
    }

    Result<void> submit_write(WriteOp, Completion<std::size_t>& completion) override {
        return hold_size(completion);
    }

    Result<void> submit_sync_data(SyncDataOp, Completion<void>& completion) override {
        return hold_void(completion);
    }

    Result<void> submit_sync_all(SyncAllOp, Completion<void>& completion) override {
        return hold_void(completion);
    }

    std::size_t poll() override { return take_terminal_count(); }

    Result<std::size_t> wait_one() override {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_condition_.wait(lock, [this] { return terminal_count_ > 0; });
        return take_terminal_count_locked();
    }

    void cancel(Completion<std::size_t>& completion) override { cancel_size(completion); }

    void cancel(Completion<void>& completion) override { cancel_void(completion); }

    std::size_t outstanding() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return held_size_.size() + held_void_.size();
    }

  private:
    Result<void> hold_size(Completion<std::size_t>& completion) {
        if (!begin_binding(completion)) {
            return sluice::make_unexpected<void>(
                sluice::IoError{sluice::IoError::Code::invalid_state});
        }
        commit_binding(completion);
        std::lock_guard<std::mutex> lock(mutex_);
        held_size_.push_back(&completion);
        return {};
    }

    Result<void> hold_void(Completion<void>& completion) {
        if (!begin_binding(completion)) {
            return sluice::make_unexpected<void>(
                sluice::IoError{sluice::IoError::Code::invalid_state});
        }
        commit_binding(completion);
        std::lock_guard<std::mutex> lock(mutex_);
        held_void_.push_back(&completion);
        return {};
    }

    void cancel_size(Completion<std::size_t>& completion) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!remove_held(held_size_, &completion)) {
                return;
            }
            ++terminal_count_;
        }
        publish(completion, sluice::make_unexpected<std::size_t>(
                                sluice::IoError{sluice::IoError::Code::canceled}));
        ready_condition_.notify_all();
    }

    void cancel_void(Completion<void>& completion) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!remove_held(held_void_, &completion)) {
                return;
            }
            ++terminal_count_;
        }
        publish(completion, sluice::make_unexpected<void>(
                                sluice::IoError{sluice::IoError::Code::canceled}));
        ready_condition_.notify_all();
    }

    template <class CompletionType>
    static bool remove_held(std::vector<CompletionType*>& held, CompletionType* target) {
        for (std::size_t i = 0; i < held.size(); ++i) {
            if (held[i] == target) {
                held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

    std::size_t take_terminal_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return take_terminal_count_locked();
    }

    std::size_t take_terminal_count_locked() {
        const std::size_t count = terminal_count_;
        terminal_count_ = 0;
        return count;
    }

    mutable std::mutex mutex_;
    std::condition_variable ready_condition_;
    std::vector<Completion<std::size_t>*> held_size_;
    std::vector<Completion<void>*> held_void_;
    std::size_t terminal_count_ = 0;
};

SLUICE_TEST(holding_backend_cancel_publishes_canceled_exactly_once) {
    AsyncIoContext context(std::make_unique<HoldingTestBackend>());

    std::array<std::byte, 8> buffer{};
    Completion<std::size_t> completion;
    auto submitted = context.submit_read(
        ReadOp{-1, buffer.data(), buffer.size(), 0}, completion);
    SLUICE_CHECK(submitted.has_value());
    SLUICE_CHECK(completion.outstanding());
    SLUICE_CHECK(context.outstanding() == 1);

    context.cancel(completion);
    SLUICE_CHECK(completion.ready());
    auto result = completion.result();
    SLUICE_CHECK(!result.has_value());
    SLUICE_CHECK(result.error().code == sluice::IoError::Code::canceled);
    SLUICE_CHECK(context.outstanding() == 0);

    context.cancel(completion);
    SLUICE_CHECK(completion.ready());
    auto result_after_second_cancel = completion.result();
    SLUICE_CHECK(!result_after_second_cancel.has_value());
    SLUICE_CHECK(result_after_second_cancel.error().code == sluice::IoError::Code::canceled);

    auto waited = context.wait_one();
    SLUICE_CHECK(waited.has_value());
    SLUICE_CHECK(waited.value() >= 1);

    completion.reset();
    SLUICE_CHECK(completion.idle());
}

} // namespace

SLUICE_TEST_MAIN()
