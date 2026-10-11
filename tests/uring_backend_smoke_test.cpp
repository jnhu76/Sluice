#include "canonical_request_test_support.hpp"
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#ifndef SLUICE_HAS_LIBURING
#error                                                                                             \
    "smoke consumer TU must receive SLUICE_HAS_LIBURING through sluice_async's public usage requirement"
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::Result;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_uring_smoke_XXXXXX";
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

bool uring_backend_available() {
    try {
        UringAsyncBackend backend;
        return backend.available();
    } catch (...) {
        return false;
    }
}

bool read_op_through_canonical_file() {
    const std::string path = make_temp_file("uring payload");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    std::vector<std::byte> dst(13);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 0});
        if (!admitted.has_value())
            return sluice::make_unexpected<std::size_t>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    if (!result.has_value())
        return false;
    if (result.value() != 13)
        return false;
    if (std::memcmp(dst.data(), "uring payload", 13) != 0)
        return false;
    return file.close().has_value();
}

bool write_op_through_canonical_file() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    const std::byte payload[5] = {std::byte{'w'}, std::byte{'r'}, std::byte{'i'}, std::byte{'t'},
                                  std::byte{'e'}};
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_write(WriteOp{file, payload, sizeof(payload), 0});
        if (!admitted.has_value())
            return sluice::make_unexpected<std::size_t>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    if (!result.has_value())
        return false;
    if (result.value() != 5)
        return false;
    std::byte observed[sizeof(payload)]{};
    if (::pread(file.native_handle(), observed, sizeof(observed), 0) !=
            static_cast<ssize_t>(sizeof(observed)) ||
        std::memcmp(observed, payload, sizeof(payload)) != 0)
        return false;
    return file.close().has_value();
}

bool sync_data_op_through_canonical_file() {
    const std::string path = make_temp_file("d");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<void> {
        AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_sync_data(SyncDataOp{file});
        if (!admitted.has_value())
            return sluice::make_unexpected<void>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    if (!result.has_value())
        return false;
    return file.close().has_value();
}

bool sync_all_op_through_canonical_file() {
    const std::string path = make_temp_file("d");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<void> {
        AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_sync_all(SyncAllOp{file});
        if (!admitted.has_value())
            return sluice::make_unexpected<void>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    if (!result.has_value())
        return false;
    return file.close().has_value();
}

bool zero_length_read_completes_zero_despite_unrepresentable_offset() {
    const std::string path = make_temp_file("d");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<std::size_t> {
        std::byte scratch{std::byte{0}};
        AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted =
            scope.submit_read(ReadOp{file, &scratch, 0, std::numeric_limits<std::uint64_t>::max()});
        if (!admitted.has_value())
            return sluice::make_unexpected<std::size_t>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    const bool ok = result.has_value() && result.value() == 0;
    return file.close().has_value() && ok;
}

bool zero_length_write_completes_zero_despite_unrepresentable_offset() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<std::size_t> {
        const std::byte scratch{std::byte{0}};
        AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_write(
            WriteOp{file, &scratch, 0, std::numeric_limits<std::uint64_t>::max()});
        if (!admitted.has_value())
            return sluice::make_unexpected<std::size_t>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    const bool ok = result.has_value() && result.value() == 0;
    return file.close().has_value() && ok;
}

bool read_with_unrepresentable_offset_rejected_at_admission() {
    const std::string path = make_temp_file("d");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<std::size_t> {
        std::byte dst{std::byte{0}};
        AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted =
            scope.submit_read(ReadOp{file, &dst, 1, std::numeric_limits<std::uint64_t>::max()});
        if (admitted.has_value()) {
            (void)scope.finish();
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{sluice::IoError::Code::backend_error});
        }
        if (ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{sluice::IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(admitted.error());
    }();

    const bool ok =
        !result.has_value() && result.error().code == sluice::IoError::Code::invalid_argument;
    return file.close().has_value() && ok;
}

bool write_with_unrepresentable_offset_rejected_at_admission() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted =
            scope.submit_write(WriteOp{file, &src, 1, std::numeric_limits<std::uint64_t>::max()});
        if (admitted.has_value()) {
            (void)scope.finish();
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{sluice::IoError::Code::backend_error});
        }
        if (ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{sluice::IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(admitted.error());
    }();

    const bool ok =
        !result.has_value() && result.error().code == sluice::IoError::Code::invalid_argument;
    return file.close().has_value() && ok;
}

} // namespace

int main() {
    if (!uring_backend_available()) {
        std::printf("NOT RUN: io_uring unavailable on this host (kernel/policy blocked); "
                    "compile+link verified, runtime probe not executed\n");
        return 0;
    }

    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"read_op_through_canonical_file", read_op_through_canonical_file},
        {"write_op_through_canonical_file", write_op_through_canonical_file},
        {"sync_data_op_through_canonical_file", sync_data_op_through_canonical_file},
        {"sync_all_op_through_canonical_file", sync_all_op_through_canonical_file},
        {"zero_length_read_completes_zero_despite_unrepresentable_offset",
         zero_length_read_completes_zero_despite_unrepresentable_offset},
        {"zero_length_write_completes_zero_despite_unrepresentable_offset",
         zero_length_write_completes_zero_despite_unrepresentable_offset},
        {"read_with_unrepresentable_offset_rejected_at_admission",
         read_with_unrepresentable_offset_rejected_at_admission},
        {"write_with_unrepresentable_offset_rejected_at_admission",
         write_with_unrepresentable_offset_rejected_at_admission},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu uring backend smoke tests passed (real liburing path)\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
