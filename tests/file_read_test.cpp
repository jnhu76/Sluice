#include "canonical_request_test_support.hpp"
#include <sluice/async/detail/threadpool_backend.hpp>
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
using sluice::FileExistence;
using sluice::FileInitialContents;
using sluice::FileOpen;
using sluice::IoError;
using sluice::make_unexpected;
using sluice::Result;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_file_read_XXXXXX";
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

bool read_returns_bytes_at_offset() {
    const std::string path = make_temp_file("hello world");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    std::vector<std::byte> dst(5);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 6});
        if (!admitted.has_value())
            return sluice::make_unexpected<std::size_t>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 5)
        return false;
    if (std::memcmp(dst.data(), "world", 5) != 0)
        return false;
    if (!file.close().has_value())
        return false;
    return true;
}

bool short_read_reports_only_available_bytes() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    std::vector<std::byte> dst(8, std::byte{0x5a});
    AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
    RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
    auto admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 1});
    if (!admitted.has_value())
        return false;
    auto result = sluice::test::wait_and_take(scope, admitted.value());
    if (!result.has_value() || result.value() != 2 || std::memcmp(dst.data(), "bc", 2) != 0)
        return false;
    for (std::size_t i = 2; i < dst.size(); ++i) {
        if (dst[i] != std::byte{0x5a})
            return false;
    }
    return file.close().has_value();
}

bool read_past_end_returns_zero() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());

    std::vector<std::byte> dst(4);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 100});
        if (!admitted.has_value())
            return sluice::make_unexpected<std::size_t>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 0)
        return false;
    return file.close().has_value();
}

bool read_empty_buffer_returns_zero_without_data_effect() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());

    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_read(ReadOp{file, nullptr, 0, 0});
        if (!r_admitted.has_value())
            return sluice::make_unexpected<std::size_t>(r_admitted.error());
        if (!r_admitted.value().valid() ||
            ctx.lookup(r_admitted.value().id()) != RequestReadiness::ready)
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        auto r = sluice::test::wait_and_take(scope, r_admitted.value());
        return r;
    }();

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 0)
        return false;
    return file.close().has_value();
}

bool outstanding_read_resource_valid_until_terminal() {
    const std::string path = make_temp_file("abcdef");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());

    std::vector<std::byte> dst(6);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 0});
        if (!admitted.has_value())
            return make_unexpected<std::size_t>(admitted.error());
        if (!admitted.value().valid() || !file.is_open())
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 6)
        return false;
    if (std::memcmp(dst.data(), "abcdef", 6) != 0)
        return false;
    if (!file.close().has_value())
        return false;
    return true;
}

bool close_releases_once_and_object_stops_representing_resource() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    if (!file.is_open())
        return false;
    const int handle = file.native_handle();
    if (!file.close().has_value())
        return false;
    if (file.is_open())
        return false;
    if (file.native_handle() >= 0)
        return false;
    if (::fcntl(handle, F_GETFD) != -1 || errno != EBADF)
        return false;
    if (!file.close().has_value())
        return false;
    return true;
}

bool moved_file_owns_nothing_and_target_keeps_resource() {
    const std::string path = make_temp_file("data");
    if (path.empty())
        return false;
    const std::string other_path = make_temp_file("");
    if (other_path.empty())
        return false;

    File file = std::move(File::open(path).value());
    File other = std::move(File::open(other_path).value());

    File target = std::move(file);
    if (file.is_open())
        return false;
    if (!target.is_open())
        return false;

    other = std::move(target);
    if (target.is_open())
        return false;
    if (!other.is_open())
        return false;

    ::unlink(path.c_str());
    ::unlink(other_path.c_str());

    std::vector<std::byte> dst(4);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(ReadOp{other, dst.data(), dst.size(), 0});
        if (!admitted.has_value())
            return sluice::make_unexpected<std::size_t>(admitted.error());
        return sluice::test::wait_and_take(scope, admitted.value());
    }();

    if (!result.has_value())
        return false;
    if (result.value() != 4)
        return false;
    if (std::memcmp(dst.data(), "data", 4) != 0)
        return false;
    return other.close().has_value();
}

bool open_create_new_rejects_existing() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;

    FileOpen mode;
    mode.existence = FileExistence::create_new;
    auto opened = File::open(path, mode);
    const bool rejected = !opened.has_value();

    auto reopened = File::open(path);
    ::unlink(path.c_str());

    if (!rejected)
        return false;
    if (!reopened.has_value())
        return false;
    return reopened.value().close().has_value();
}

bool truncate_requires_writable_access() {
    const std::string path = make_temp_file("keep");
    if (path.empty())
        return false;

    FileOpen mode;
    mode.contents = FileInitialContents::truncate;
    auto opened = File::open(path, mode);

    const bool preserved = file_content_is(path, "keep");
    ::unlink(path.c_str());

    if (opened.has_value())
        return false;
    if (opened.error().code != IoError::Code::invalid_argument)
        return false;
    if (!preserved)
        return false;
    return true;
}

bool truncate_replaces_contents_for_writable_access() {
    const std::string path = make_temp_file("keep");
    if (path.empty())
        return false;

    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.contents = FileInitialContents::truncate;
    auto opened = File::open(path, mode);

    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    if (!file_content_is(path, ""))
        return false;
    return file.close().has_value();
}

bool read_after_close_reports_invalid_state() {
    const std::string path = make_temp_file("abcd");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    if (!file.close().has_value())
        return false;

    std::vector<std::byte> dst(4);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 0});
        if (admitted.has_value() || ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(admitted.error());
    }();

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_state)
        return false;
    return true;
}

bool scoped_read_on_write_only_file_rejected_upfront() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    FileOpen mode;
    mode.access = FileAccess::write_only;
    File file = std::move(File::open(path, mode).value());
    ::unlink(path.c_str());

    std::vector<std::byte> dst(3);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 0});
        if (r_admitted.has_value() || ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(r_admitted.error());
    }();

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_argument)
        return false;

    auto empty_result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_read(ReadOp{file, nullptr, 0, 0});
        if (r_admitted.has_value() || ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(r_admitted.error());
    }();

    if (empty_result.has_value())
        return false;
    if (empty_result.error().code != IoError::Code::invalid_argument)
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
        {"read_returns_bytes_at_offset", read_returns_bytes_at_offset},
        {"read_past_end_returns_zero", read_past_end_returns_zero},
        {"short_read_reports_only_available_bytes", short_read_reports_only_available_bytes},
        {"read_empty_buffer_returns_zero_without_data_effect",
         read_empty_buffer_returns_zero_without_data_effect},
        {"outstanding_read_resource_valid_until_terminal",
         outstanding_read_resource_valid_until_terminal},
        {"close_releases_once_and_object_stops_representing_resource",
         close_releases_once_and_object_stops_representing_resource},
        {"moved_file_owns_nothing_and_target_keeps_resource",
         moved_file_owns_nothing_and_target_keeps_resource},
        {"open_create_new_rejects_existing", open_create_new_rejects_existing},
        {"truncate_requires_writable_access", truncate_requires_writable_access},
        {"truncate_replaces_contents_for_writable_access",
         truncate_replaces_contents_for_writable_access},
        {"read_after_close_reports_invalid_state", read_after_close_reports_invalid_state},
        {"scoped_read_on_write_only_file_rejected_upfront",
         scoped_read_on_write_only_file_rejected_upfront},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu file read tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
