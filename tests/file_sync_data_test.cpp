#include <sluice/async/file.hpp>
#include <sluice/async/task_result.hpp>
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
using sluice::Result;
using sluice::make_unexpected;

FileOpen writable_mode() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_file_sync_data_XXXXXX";
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

bool sync_data_after_write_succeeds() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    const std::string src_str = "XYZ";
    auto result = run_task_to_result<void>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<void>>& slot) {
            Completion<std::size_t> wc;
            auto wr = await_write_at(file, ctx, 0, as_bytes(src_str), wc);
            if (!wr.has_value() || wr.value() != 3) {
                slot.publish(make_unexpected<void>(IoError{IoError::Code::invalid_state}));
                return;
            }
            Completion<void> c;
            auto sr = await_sync_data(file, ctx, c);
            if (!c.idle()) {
                slot.publish(make_unexpected<void>(IoError{IoError::Code::invalid_state}));
                return;
            }
            slot.publish(sr);
        });

    const bool content_ok = file_content_is(path, "XYZ");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool sync_data_completion_is_reusable() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    auto result = run_task_to_result<void>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<void>>& slot) {
            Completion<void> c;
            auto r1 = await_sync_data(file, ctx, c);
            if (!r1.has_value() || !c.idle()) {
                slot.publish(make_unexpected<void>(IoError{IoError::Code::invalid_state}));
                return;
            }
            slot.publish(await_sync_data(file, ctx, c));
        });

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    return file.close().has_value();
}

bool sync_data_on_read_only_file_succeeds() {
    const std::string path = make_temp_file("keep");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());

    auto result = run_task_to_result<void>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<void>>& slot) {
            Completion<void> c;
            auto r = await_sync_data(file, ctx, c);
            if (!c.idle()) {
                slot.publish(make_unexpected<void>(IoError{IoError::Code::invalid_state}));
                return;
            }
            slot.publish(r);
        });

    const bool content_ok = file_content_is(path, "keep");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool sync_data_on_write_only_file_succeeds() {
    const std::string path = make_temp_file("keep");
    if (path.empty())
        return false;
    FileOpen mode;
    mode.access = FileAccess::write_only;
    File file = std::move(File::open(path, mode).value());

    auto result = run_task_to_result<void>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<void>>& slot) {
            Completion<void> c;
            auto r = await_sync_data(file, ctx, c);
            if (!c.idle()) {
                slot.publish(make_unexpected<void>(IoError{IoError::Code::invalid_state}));
                return;
            }
            slot.publish(r);
        });

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    return file.close().has_value();
}

bool sync_data_after_close_reports_invalid_state() {
    const std::string path = make_temp_file("abcd");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    if (!file.close().has_value())
        return false;

    auto result = run_task_to_result<void>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<void>>& slot) {
            Completion<void> c;
            auto r = await_sync_data(file, ctx, c);
            if (!c.idle()) {
                slot.publish(make_unexpected<void>(IoError{IoError::Code::invalid_state}));
                return;
            }
            slot.publish(r);
        });

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_state)
        return false;
    return true;
}

bool sync_data_on_device_reports_backend_error() {
    File file = std::move(File::open("/dev/null").value());
    if (!file.is_open())
        return false;

    auto result = run_task_to_result<void>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<void>>& slot) {
            Completion<void> c;
            auto r = await_sync_data(file, ctx, c);
            if (!c.idle()) {
                slot.publish(make_unexpected<void>(IoError{IoError::Code::invalid_state}));
                return;
            }
            slot.publish(r);
        });

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::backend_error)
        return false;
    if (result.error().os_errno != EINVAL)
        return false;
    return file.close().has_value();
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"sync_data_after_write_succeeds", sync_data_after_write_succeeds},
        {"sync_data_completion_is_reusable", sync_data_completion_is_reusable},
        {"sync_data_on_read_only_file_succeeds", sync_data_on_read_only_file_succeeds},
        {"sync_data_on_write_only_file_succeeds", sync_data_on_write_only_file_succeeds},
        {"sync_data_after_close_reports_invalid_state", sync_data_after_close_reports_invalid_state},
        {"sync_data_on_device_reports_backend_error", sync_data_on_device_reports_backend_error},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu file sync data tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
