#include "canonical_request_test_support.hpp"
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
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

FileOpen writable_mode() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_file_sync_all_XXXXXX";
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

std::span<const std::byte> as_bytes(const std::string& src) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(src.data()), src.size());
}

bool sync_all_after_write_succeeds() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    const std::string src_str = "XYZ";
    auto result = [&]() -> Result<void> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto wr_admitted = scope.submit_write(
            WriteOp{file, as_bytes(src_str).data(), as_bytes(src_str).size(), 0});
        if (!wr_admitted.has_value())
            return sluice::make_unexpected<void>(wr_admitted.error());
        auto wr = sluice::test::wait_and_take(scope, wr_admitted.value());
        if (!wr.has_value() || wr.value() != 3) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        auto sr_admitted = scope.submit_sync_all(SyncAllOp{file});
        if (!sr_admitted.has_value())
            return sluice::make_unexpected<void>(sr_admitted.error());
        auto sr = sluice::test::wait_and_take(scope, sr_admitted.value());
        return sr;
    }();

    const bool content_ok = file_content_is(path, "XYZ");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool sync_all_successive_requests_settle() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    auto result = [&]() -> Result<void> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r1_admitted = scope.submit_sync_all(SyncAllOp{file});
        if (!r1_admitted.has_value())
            return sluice::make_unexpected<void>(r1_admitted.error());
        auto r1 = sluice::test::wait_and_take(scope, r1_admitted.value());
        if (!r1.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        auto admitted = scope.submit_sync_all(SyncAllOp{file});
        if (!admitted.has_value())
            return sluice::make_unexpected<void>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    return file.close().has_value();
}

bool sync_all_on_read_only_file_succeeds() {
    const std::string path = make_temp_file("keep");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());

    auto result = [&]() -> Result<void> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_sync_all(SyncAllOp{file});
        if (!r_admitted.has_value())
            return sluice::make_unexpected<void>(r_admitted.error());
        auto r = sluice::test::wait_and_take(scope, r_admitted.value());
        return r;
    }();

    const bool content_ok = file_content_is(path, "keep");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool sync_all_on_write_only_file_succeeds() {
    const std::string path = make_temp_file("keep");
    if (path.empty())
        return false;
    FileOpen mode;
    mode.access = FileAccess::write_only;
    File file = std::move(File::open(path, mode).value());

    auto result = [&]() -> Result<void> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_sync_all(SyncAllOp{file});
        if (!r_admitted.has_value())
            return sluice::make_unexpected<void>(r_admitted.error());
        auto r = sluice::test::wait_and_take(scope, r_admitted.value());
        return r;
    }();

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    return file.close().has_value();
}

bool sync_all_after_close_reports_invalid_state() {
    const std::string path = make_temp_file("abcd");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    if (!file.close().has_value())
        return false;

    auto result = [&]() -> Result<void> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_sync_all(SyncAllOp{file});
        if (r_admitted.has_value() || ctx.outstanding() != 0)
            return sluice::make_unexpected<void>(IoError{IoError::Code::backend_error});
        return sluice::make_unexpected<void>(r_admitted.error());
    }();

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_state)
        return false;
    return true;
}

bool sync_all_on_device_reports_backend_error() {
    File file = std::move(File::open("/dev/null").value());
    if (!file.is_open())
        return false;

    auto result = [&]() -> Result<void> {
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_sync_all(SyncAllOp{file});
        if (!r_admitted.has_value())
            return sluice::make_unexpected<void>(r_admitted.error());
        auto r = sluice::test::wait_and_take(scope, r_admitted.value());
        return r;
    }();

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::backend_error)
        return false;
    if (result.error().os_errno != EINVAL)
        return false;
    return file.close().has_value();
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"sync_all_after_write_succeeds", sync_all_after_write_succeeds},
        {"sync_all_successive_requests_settle", sync_all_successive_requests_settle},
        {"sync_all_on_read_only_file_succeeds", sync_all_on_read_only_file_succeeds},
        {"sync_all_on_write_only_file_succeeds", sync_all_on_write_only_file_succeeds},
        {"sync_all_after_close_reports_invalid_state", sync_all_after_close_reports_invalid_state},
        {"sync_all_on_device_reports_backend_error", sync_all_on_device_reports_backend_error},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu file sync all tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
