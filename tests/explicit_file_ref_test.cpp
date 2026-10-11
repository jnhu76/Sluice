#include "canonical_request_test_support.hpp"
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::make_unexpected;
using sluice::Result;

template <class Op, class... Args>
auto brace_init_detects(int) -> decltype(Op{std::declval<Args>()...}, std::true_type{});
template <class Op, class... Args> auto brace_init_detects(long) -> std::false_type;

static_assert(
    !decltype(brace_init_detects<ReadOp, int, std::byte*, std::size_t, std::uint64_t>(0))::value,
    "ReadOp must not be constructible from a bare fd");
static_assert(!decltype(brace_init_detects<WriteOp, int, const std::byte*, std::size_t,
                                           std::uint64_t>(0))::value,
              "WriteOp must not be constructible from a bare fd");
static_assert(!decltype(brace_init_detects<SyncDataOp, int>(0))::value,
              "SyncDataOp must not be constructible from a bare fd");
static_assert(!decltype(brace_init_detects<SyncAllOp, int>(0))::value,
              "SyncAllOp must not be constructible from a bare fd");
static_assert(!std::is_convertible_v<int, NativeFileRef>,
              "a bare int does not silently become an operation resource reference");
static_assert(std::is_constructible_v<NativeFileRef, int, FileAccess>,
              "NativeFileRef{int, declared access} is the explicitly-named interop path");
static_assert(std::is_convertible_v<File&, NativeFileRef>,
              "canonical File converts implicitly to NativeFileRef");
static_assert(std::is_trivially_copyable_v<NativeFileRef>,
              "NativeFileRef is a value copy of the handle, owning nothing");

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_explicit_ref_XXXXXX";
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

FileOpen writable_mode() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

bool file_content_is(const std::string& path, const std::string& expected) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    std::vector<char> buf(expected.size() + 1, '\0');
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    ::close(fd);
    if (n != static_cast<ssize_t>(expected.size()))
        return false;
    return std::memcmp(buf.data(), expected.data(), expected.size()) == 0;
}

bool multiple_outstanding_ops_on_one_canonical_file() {
    const std::string path = make_temp_file("hello world!!");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    std::vector<std::byte> head(5);
    std::vector<std::byte> tail(5);
    const std::string patch = "XYZ";
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 3, ScopeCleanupPolicy::drain);
        auto r1 = scope.submit_read(ReadOp{file, head.data(), head.size(), 0});
        if (!r1.has_value())
            return make_unexpected<std::size_t>(r1.error());
        auto r2 = scope.submit_read(ReadOp{file, tail.data(), tail.size(), 6});
        if (!r2.has_value())
            return make_unexpected<std::size_t>(r2.error());
        auto r3 = scope.submit_write(
            WriteOp{file, reinterpret_cast<const std::byte*>(patch.data()), patch.size(), 11});
        if (!r3.has_value())
            return make_unexpected<std::size_t>(r3.error());
        auto head_result = sluice::test::wait_and_take(scope, r1.value());
        auto tail_result = sluice::test::wait_and_take(scope, r2.value());
        auto write_result = sluice::test::wait_and_take(scope, r3.value());
        if (!head_result.has_value() || !tail_result.has_value() || !write_result.has_value())
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        if (head_result.value() != 5 || tail_result.value() != 5 || write_result.value() != 3)
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        return head_result.value() + tail_result.value() + write_result.value();
    }();

    const bool content_ok = file_content_is(path, "hello worldXYZ");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 13)
        return false;
    if (std::memcmp(head.data(), "hello", 5) != 0)
        return false;
    if (std::memcmp(tail.data(), "world", 5) != 0)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool explicit_native_ref_over_raw_fd() {
    const std::string path = make_temp_file("interop payload");
    if (path.empty())
        return false;
    const int raw_fd = ::open(path.c_str(), O_RDONLY);
    if (raw_fd < 0)
        return false;

    std::vector<std::byte> dst(7);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(
            ReadOp{NativeFileRef{raw_fd, FileAccess::read_only}, dst.data(), dst.size(), 8});
        if (!admitted.has_value())
            return make_unexpected<std::size_t>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    ::close(raw_fd);
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 7)
        return false;
    return std::memcmp(dst.data(), "payload", 7) == 0;
}

bool cancel_through_file_referenced_op_reaches_terminal() {
    const std::string path = make_temp_file("cancel target");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());

    bool terminal_observed = false;
    bool follow_up_read_ok = false;

    std::vector<std::byte> dst(4);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 0});
        if (!admitted.has_value())
            return make_unexpected<std::size_t>(admitted.error());
        auto canceled = ctx.cancel(admitted.value().id());
        if (!canceled.has_value())
            return make_unexpected<std::size_t>(canceled.error());
        auto terminal = sluice::test::wait_and_take(scope, admitted.value());
        if (!terminal.has_value() && terminal.error().code != IoError::Code::canceled)
            return make_unexpected<std::size_t>(terminal.error());
        terminal_observed = ctx.lookup(admitted.value().id()) == RequestReadiness::empty;
        auto follow = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 7});
        if (!follow.has_value())
            return make_unexpected<std::size_t>(follow.error());
        auto follow_result = sluice::test::wait_and_take(scope, follow.value());
        follow_up_read_ok = follow_result.has_value() && follow_result.value() == 4 &&
                            std::memcmp(dst.data(), "targ", 4) == 0;
        return std::size_t{0};
    }();

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (!terminal_observed)
        return false;
    if (!follow_up_read_ok)
        return false;
    return file.close().has_value();
}

bool file_derived_ref_rejected_at_submit_on_wrong_access() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    FileOpen mode;
    mode.access = FileAccess::write_only;
    File file = std::move(File::open(path, mode).value());
    ::unlink(path.c_str());

    std::vector<std::byte> dst(3);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 0});
        if (admitted.has_value()) {
            (void)scope.finish();
            return std::size_t{0};
        }
        if (ctx.outstanding() != 0)
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        return make_unexpected<std::size_t>(admitted.error());
    }();

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_argument)
        return false;
    return file.close().has_value();
}

bool file_derived_write_ref_rejected_at_submit_on_read_only_file() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    const std::byte payload{0x21};
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_write(WriteOp{file, &payload, 1, 0});
        if (admitted.has_value()) {
            (void)scope.finish();
            return std::size_t{0};
        }
        if (ctx.outstanding() != 0)
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        return make_unexpected<std::size_t>(admitted.error());
    }();

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_argument)
        return false;
    return file.close().has_value();
}

bool closed_file_derived_ref_rejected_as_invalid_state() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    std::vector<std::byte> dst(3);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 0});
        if (admitted.has_value()) {
            (void)scope.finish();
            return std::size_t{0};
        }
        if (ctx.outstanding() != 0)
            return make_unexpected<std::size_t>(IoError{IoError::Code::backend_error});
        return make_unexpected<std::size_t>(admitted.error());
    }();

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_state)
        return false;
    return true;
}

bool zero_length_op_succeeds_before_offset_validation() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted =
            scope.submit_read(ReadOp{file, nullptr, 0, std::numeric_limits<std::uint64_t>::max()});
        if (!admitted.has_value())
            return make_unexpected<std::size_t>(admitted.error());
        if (!admitted.value().valid())
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    if (!result.has_value())
        return false;
    if (result.value() != 0)
        return false;
    return file.close().has_value();
}

bool interop_declared_access_is_a_claim_not_validation() {
    const std::string path = make_temp_file("xyz");
    if (path.empty())
        return false;
    const int raw_fd = ::open(path.c_str(), O_RDONLY);
    if (raw_fd < 0)
        return false;

    const std::byte payload{0x7E};
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_write(
            WriteOp{NativeFileRef{raw_fd, FileAccess::read_write}, &payload, 1, 0});
        if (!admitted.has_value())
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_argument});
        auto terminal = sluice::test::wait_and_take(scope, admitted.value());
        if (terminal.has_value() || terminal.error().code != IoError::Code::backend_error ||
            terminal.error().os_errno != EBADF)
            return std::size_t{0};
        return terminal;
    }();

    ::close(raw_fd);
    ::unlink(path.c_str());

    if (result.has_value())
        return false;
    if (result.error().code == IoError::Code::invalid_argument)
        return false;
    return true;
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"multiple_outstanding_ops_on_one_canonical_file",
         multiple_outstanding_ops_on_one_canonical_file},
        {"explicit_native_ref_over_raw_fd", explicit_native_ref_over_raw_fd},
        {"cancel_through_file_referenced_op_reaches_terminal",
         cancel_through_file_referenced_op_reaches_terminal},
        {"file_derived_ref_rejected_at_submit_on_wrong_access",
         file_derived_ref_rejected_at_submit_on_wrong_access},
        {"file_derived_write_ref_rejected_at_submit_on_read_only_file",
         file_derived_write_ref_rejected_at_submit_on_read_only_file},
        {"interop_declared_access_is_a_claim_not_validation",
         interop_declared_access_is_a_claim_not_validation},
        {"closed_file_derived_ref_rejected_as_invalid_state",
         closed_file_derived_ref_rejected_as_invalid_state},
        {"zero_length_op_succeeds_before_offset_validation",
         zero_length_op_succeeds_before_offset_validation},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu explicit file ref tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
