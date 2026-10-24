// #575 research oracle: the internal submit-flush counter must count real
// transport flush attempts. The existing suite only asserts its zero value
// (no-flush paths); this test pins the positive semantics — every accepted
// non-zero-length data submission produces exactly one transport flush
// attempt, monotonicity holds across submissions, and zero-length requests
// never flush — so a mutant that suppresses the increment after a real
// submission is killed by a counter assertion, not by an unrelated failure.

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/test_access.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

#ifndef SLUICE_HAS_LIBURING
#error "the uring flush-count oracle must receive SLUICE_HAS_LIBURING"
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::async::detail::RequestCore;
using sluice::async::detail::TestAccess;

int fake_submit_no_kernel(void*, ::io_uring* ring) noexcept {
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(ready);
}

struct Tracker {
    const char* name;
    int failures = 0;

    void check(bool ok, const char* label) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s\n", name, label);
        }
    }
};

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_575_flush_XXXXXX";
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

void drive_completion(AsyncIoContext& ctx, UringAsyncBackend& backend,
                      std::uint64_t offset, int res) {
    (void)ctx.poll();
    const auto cookie = backend.live_cookie_for_offset_for_test(offset);
    if (cookie.has_value())
        backend.inject_cqe_for_test(*cookie, res);
    (void)ctx.poll();
}

bool poll_until_settled(AsyncIoContext& ctx, Request<std::size_t>& request) {
    for (int i = 0; i < 64 && !request.ready(); ++i)
        (void)ctx.poll();
    if (!request.ready())
        return false;
    (void)request.take_result();
    RequestCore& core = *TestAccess::context_core(ctx);
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    return core.snapshot().accepted_live == 0;
}

void single_submission_counts_one_flush(Tracker& t) {
    UringBackendSubmitTestHooks hooks;
    hooks.submit = fake_submit_no_kernel;
    auto owned = std::make_unique<UringAsyncBackend>(UringConfig{4, 8}, hooks);
    UringAsyncBackend* raw = owned.get();
    AsyncIoContext ctx{sluice::async::detail::make_context(std::move(owned))};
    auto file = open_temp_file(t, "12345678");
    if (!file.has_value())
        return;

    t.check(raw->submit_flushes_for_test() == 0, "a fresh backend has flushed nothing");

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(
        ReadOp{NativeFileRef(*file), buffer.data(), buffer.size(), 0});
    t.check(submitted.has_value(), "the data submission is accepted");
    t.check(raw->submit_flushes_for_test() == 1,
            "one accepted data submission flushes transport exactly once");

    drive_completion(ctx, *raw, 0, 4);
    const bool completed = poll_until_settled(ctx, submitted.value());
    t.check(completed, "the request completes");
    t.check(raw->submit_flushes_for_test() == 1,
            "completion and polling add no further flush attempts");
}

void sequential_submissions_are_monotonic(Tracker& t) {
    UringBackendSubmitTestHooks hooks;
    hooks.submit = fake_submit_no_kernel;
    auto owned = std::make_unique<UringAsyncBackend>(UringConfig{4, 8}, hooks);
    UringAsyncBackend* raw = owned.get();
    AsyncIoContext ctx{sluice::async::detail::make_context(std::move(owned))};
    auto file = open_temp_file(t, "12345678");
    if (!file.has_value())
        return;

    std::vector<std::byte> buffer(4, std::byte{0});
    for (std::uint64_t offset = 0; offset < 2; ++offset) {
        auto submitted = ctx.submit_read(
            ReadOp{NativeFileRef(*file), buffer.data(), buffer.size(), offset * 4});
        t.check(submitted.has_value(), "the submission is accepted");
        drive_completion(ctx, *raw, offset * 4, 4);
        const bool completed = poll_until_settled(ctx, submitted.value());
        t.check(completed, "the request completes");
    }
    t.check(raw->submit_flushes_for_test() == 2,
            "two sequential data submissions flush transport exactly twice");
}

void zero_length_request_never_flushes(Tracker& t) {
    UringBackendSubmitTestHooks hooks;
    hooks.submit = fake_submit_no_kernel;
    auto owned = std::make_unique<UringAsyncBackend>(UringConfig{4, 8}, hooks);
    UringAsyncBackend* raw = owned.get();
    AsyncIoContext ctx{sluice::async::detail::make_context(std::move(owned))};
    auto file = open_temp_file(t, "12345678");
    if (!file.has_value())
        return;

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 0, 0});
    t.check(submitted.has_value(), "the zero-length submission is accepted");
    const bool completed = poll_until_settled(ctx, submitted.value());
    t.check(completed, "the zero-length request completes inline");
    t.check(raw->submit_flushes_for_test() == 0,
            "a zero-length request never flushes transport");
}

} // namespace

int main() {
    Tracker t{"uring_flush_count_oracle"};
    single_submission_counts_one_flush(t);
    sequential_submissions_are_monotonic(t);
    zero_length_request_never_flushes(t);
    if (t.failures == 0) {
        std::printf("[uring_flush_count_oracle_test] all checks passed\n");
        return 0;
    }
    std::printf("[uring_flush_count_oracle_test] %d check(s) failed\n", t.failures);
    return 1;
}
