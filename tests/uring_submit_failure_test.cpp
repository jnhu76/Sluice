#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/error.hpp>

#include <liburing.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using sluice::IoError;
using sluice::async::Completion;
using sluice::async::UringAsyncBackend;
using sluice::async::UringBackendSubmitTestHooks;
using sluice::async::WriteOp;

constexpr int kRealSubmit = std::numeric_limits<int>::max();
constexpr int kSubmitOne = std::numeric_limits<int>::max() - 1;

class TempFile {
public:
    TempFile() {
        path_ = (std::filesystem::temp_directory_path() /
                 "sluice_uring_submit_failure_XXXXXX")
                    .string();
        path_.push_back('\0');
        fd_ = ::mkstemp(path_.data());
        path_.pop_back();
    }

    ~TempFile() {
        if (fd_ >= 0) (void)::close(fd_);
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    int fd() const noexcept { return fd_; }

private:
    int fd_ = -1;
    std::string path_;
};

class SubmitScript {
public:
    explicit SubmitScript(std::span<const int> steps) : steps_(steps) {}

    static int invoke(void* context, io_uring* ring) noexcept {
        auto& self = *static_cast<SubmitScript*>(context);
        ++self.calls_;
        if (self.next_ >= self.steps_.size()) return -EIO;
        const int step = self.steps_[self.next_++];
        if (step == kRealSubmit) return ::io_uring_submit(ring);
        if (step == kSubmitOne) return submit_prefix(ring, 1);
        return step;
    }

    std::size_t calls() const noexcept { return calls_; }

private:
    static int submit_prefix(io_uring* ring, unsigned count) noexcept {
        io_uring_sq& sq = ring->sq;
        const unsigned head = io_uring_load_sq_head(ring);
        const unsigned available = sq.sqe_tail - head;
        if (count == 0 || count > available) return -EINVAL;

        const unsigned published_tail = head + count;
        sq.sqe_head = published_tail;
        io_uring_smp_store_release(sq.ktail, published_tail);
        return ::io_uring_enter(ring->enter_ring_fd, count, 0, 0, nullptr);
    }

    std::span<const int> steps_;
    std::size_t next_ = 0;
    std::size_t calls_ = 0;
};

UringBackendSubmitTestHooks hooks_for(SubmitScript& script) {
    return UringBackendSubmitTestHooks{&script, &SubmitScript::invoke};
}

template <class Predicate>
std::size_t poll_bounded(UringAsyncBackend& backend, Predicate done) {
    std::size_t completed = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        completed += backend.poll();
        std::this_thread::yield();
    }
    return completed;
}

}  // namespace

SLUICE_TEST_CASE(uring_submit_transient_error_recovers_on_next_poll) {
    constexpr std::array steps{-EINTR, kRealSubmit};
    SubmitScript script(steps);
    UringAsyncBackend backend(8, hooks_for(script));
    if (!backend.available()) return;

    TempFile file;
    std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2},
                                   std::byte{3}, std::byte{4}};
    Completion<std::size_t> completion;
    SLUICE_CHECK(
        backend.submit_write(
                   WriteOp{file.fd(), bytes.data(), bytes.size(), 0}, completion)
            .has_value());

    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(!completion.ready());
    SLUICE_CHECK(backend.outstanding() == 1);

    const std::size_t completed =
        poll_bounded(backend, [&] { return completion.ready(); });
    SLUICE_CHECK(completed == 1);
    SLUICE_CHECK(completion.ready());
    SLUICE_CHECK(completion.result().has_value());
    SLUICE_CHECK(script.calls() == 2);
    SLUICE_CHECK(backend.outstanding() == 0);
}

SLUICE_TEST_CASE(uring_submit_zero_progress_recovers_without_duplication) {
    constexpr std::array steps{0, kRealSubmit};
    SubmitScript script(steps);
    UringAsyncBackend backend(8, hooks_for(script));
    if (!backend.available()) return;

    TempFile file;
    std::array<std::byte, 4> bytes{};
    Completion<std::size_t> completion;
    SLUICE_CHECK(
        backend.submit_write(
                   WriteOp{file.fd(), bytes.data(), bytes.size(), 0}, completion)
            .has_value());

    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(!completion.ready());
    const std::size_t completed =
        poll_bounded(backend, [&] { return completion.ready(); });
    SLUICE_CHECK(completed == 1);
    SLUICE_CHECK(completion.result().has_value());
    SLUICE_CHECK(script.calls() == 2);
    SLUICE_CHECK(backend.poll() == 0);
}

SLUICE_TEST_CASE(uring_submit_permanent_error_completes_unsubmitted_once) {
    constexpr std::array steps{-EIO};
    SubmitScript script(steps);
    UringAsyncBackend backend(8, hooks_for(script));
    if (!backend.available()) return;

    TempFile file;
    std::array<std::byte, 4> bytes{};
    Completion<std::size_t> completion;
    SLUICE_CHECK(
        backend.submit_write(
                   WriteOp{file.fd(), bytes.data(), bytes.size(), 0}, completion)
            .has_value());

    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(completion.ready());
    SLUICE_CHECK(!completion.result().has_value());
    SLUICE_CHECK(completion.result().error().code ==
                 IoError::Code::backend_error);
    SLUICE_CHECK(completion.result().error().os_errno == EIO);
    SLUICE_CHECK(backend.outstanding() == 0);

    // Fatal is terminal: no retry, no duplicate completion, and later submits
    // synchronously expose the first backend error.
    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(script.calls() == 1);
    Completion<std::size_t> later;
    auto later_result = backend.submit_write(
        WriteOp{file.fd(), bytes.data(), bytes.size(), 8}, later);
    SLUICE_CHECK(!later_result.has_value());
    SLUICE_CHECK(later_result.error().os_errno == EIO);
    SLUICE_CHECK(later.idle());
    auto wait_result = backend.wait_one();
    SLUICE_CHECK(!wait_result.has_value());
    SLUICE_CHECK(wait_result.error().os_errno == EIO);
}

SLUICE_TEST_CASE(uring_submit_repeated_zero_progress_becomes_terminal) {
    constexpr std::array steps{0, 0, kRealSubmit};
    SubmitScript script(steps);
    UringAsyncBackend backend(8, hooks_for(script));
    if (!backend.available()) return;

    TempFile file;
    std::array<std::byte, 4> bytes{};
    Completion<std::size_t> completion;
    SLUICE_CHECK(
        backend.submit_write(
                   WriteOp{file.fd(), bytes.data(), bytes.size(), 0}, completion)
            .has_value());

    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(completion.ready());
    SLUICE_CHECK(!completion.result().has_value());
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(script.calls() == 2);
}

SLUICE_TEST_CASE(uring_submit_partial_then_error_splits_kernel_ownership) {
    constexpr std::array steps{kSubmitOne, -EIO};
    SubmitScript script(steps);
    UringAsyncBackend backend(8, hooks_for(script));
    if (!backend.available()) return;

    TempFile file;
    std::array<std::byte, 4> first{std::byte{0x11}, std::byte{0x12},
                                  std::byte{0x13}, std::byte{0x14}};
    std::array<std::byte, 4> second{std::byte{0x21}, std::byte{0x22},
                                   std::byte{0x23}, std::byte{0x24}};
    Completion<std::size_t> first_completion;
    Completion<std::size_t> second_completion;
    SLUICE_CHECK(
        backend.submit_write(
                   WriteOp{file.fd(), first.data(), first.size(), 0},
                   first_completion)
            .has_value());
    SLUICE_CHECK(
        backend.submit_write(
                   WriteOp{file.fd(), second.data(), second.size(), 8},
                   second_completion)
            .has_value());

    std::size_t completed = backend.poll();  // kernel accepts only the first SQE
    completed += backend.poll();             // permanent failure for the remainder
    completed += poll_bounded(
        backend, [&] { return first_completion.ready(); });

    SLUICE_CHECK(first_completion.ready());
    SLUICE_CHECK(first_completion.result().has_value());
    SLUICE_CHECK(first_completion.result().value() == first.size());
    SLUICE_CHECK(second_completion.ready());
    SLUICE_CHECK(!second_completion.result().has_value());
    SLUICE_CHECK(second_completion.result().error().os_errno == EIO);
    SLUICE_CHECK(completed == 2);
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(script.calls() == 2);

    std::array<std::byte, 12> on_disk{};
    const ssize_t read_count =
        ::pread(file.fd(), on_disk.data(), on_disk.size(), 0);
    SLUICE_CHECK(read_count == static_cast<ssize_t>(first.size()));
}

SLUICE_TEST_CASE(uring_submit_fatal_drops_unsubmitted_cancel_only) {
    constexpr std::array steps{kSubmitOne, -EIO};
    SubmitScript script(steps);
    UringAsyncBackend backend(8, hooks_for(script));
    if (!backend.available()) return;

    TempFile file;
    std::array<std::byte, 4> bytes{std::byte{0x31}, std::byte{0x32},
                                   std::byte{0x33}, std::byte{0x34}};
    Completion<std::size_t> completion;
    SLUICE_CHECK(
        backend.submit_write(
                   WriteOp{file.fd(), bytes.data(), bytes.size(), 0}, completion)
            .has_value());
    backend.cancel(completion);

    std::size_t completed = backend.poll();  // submit operation, leave cancel
    completed += backend.poll();             // fail only the cancel SQE
    completed +=
        poll_bounded(backend, [&] { return completion.ready(); });

    SLUICE_CHECK(completion.ready());
    SLUICE_CHECK(completion.result().has_value());
    SLUICE_CHECK(completion.result().value() == bytes.size());
    SLUICE_CHECK(completed == 1);
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(script.calls() == 2);
    SLUICE_CHECK(backend.poll() == 0);
}

SLUICE_MAIN()
