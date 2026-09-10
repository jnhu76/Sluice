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
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileInitialContents;
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
    char path[] = "/tmp/sluice_file_write_XXXXXX";
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

bool file_size_is(const std::string& path, off_t expected) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return false;
    return st.st_size == expected;
}

bool byte_at_offset_is(const std::string& path, off_t offset, char expected) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    char byte = '\0';
    const ssize_t n = ::pread(fd, &byte, 1, offset);
    ::close(fd);
    return n == 1 && byte == expected;
}

std::span<const std::byte> as_bytes(const std::string& src) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(src.data()), src.size());
}

bool write_replaces_bytes_at_offset() {
    const std::string path = make_temp_file("abcdef");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    const std::string src_str = "XYZ";
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            slot.publish(await_write_at(file, ctx, 2, as_bytes(src_str), c));
        });

    const bool content_ok = file_content_is(path, "abXYZf");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 3)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool write_can_extend_file() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    const std::string src_str = "Z";
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            slot.publish(await_write_at(file, ctx, 5, as_bytes(src_str), c));
        });

    const bool size_ok = file_size_is(path, 6);
    const bool byte_ok = byte_at_offset_is(path, 5, 'Z');
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 1)
        return false;
    if (!size_ok)
        return false;
    if (!byte_ok)
        return false;
    return file.close().has_value();
}

bool write_empty_buffer_returns_zero_without_operation() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto r = await_write_at(file, ctx, 0, std::span<const std::byte>{}, c);
            if (!c.idle()) {
                slot.publish(make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state}));
                return;
            }
            slot.publish(r);
        });

    const bool content_ok = file_content_is(path, "abc");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 0)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool write_after_close_reports_invalid_state() {
    const std::string path = make_temp_file("abcd");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    if (!file.close().has_value())
        return false;

    const std::string src_str = "X";
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto r = await_write_at(file, ctx, 0, as_bytes(src_str), c);
            if (!c.idle()) {
                slot.publish(make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state}));
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

bool write_with_read_only_access_fails() {
    const std::string path = make_temp_file("keep");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());

    const std::string src_str = "X";
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto r = await_write_at(file, ctx, 0, as_bytes(src_str), c);
            if (!c.idle()) {
                slot.publish(make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state}));
                return;
            }
            slot.publish(r);
        });

    const bool content_ok = file_content_is(path, "keep");
    ::unlink(path.c_str());

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_argument)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool write_result_reports_bytes_written() {
    const std::string path = make_temp_file("hello");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    const std::string src_str = "HELLO";
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            slot.publish(await_write_at(file, ctx, 0, as_bytes(src_str), c));
        });

    const bool content_ok = file_content_is(path, "HELLO");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != src_str.size())
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool write_keeps_resource_and_buffer_valid_until_terminal() {
    const std::string path = make_temp_file("abcdef");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    const std::string src_str = "XYZ";
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto sr = ctx.submit_write(
                WriteOp{file.native_handle(), as_bytes(src_str).data(), src_str.size(), 2}, c);
            if (!sr.has_value()) {
                slot.publish(make_unexpected<std::size_t>(sr.error()));
                return;
            }
            if (!c.outstanding()) {
                slot.publish(make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state}));
                return;
            }
            if (!file.is_open()) {
                slot.publish(make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state}));
                return;
            }
            auto wr = ctx.await_completion(c);
            if (!wr.has_value()) {
                slot.publish(make_unexpected<std::size_t>(wr.error()));
                return;
            }
            auto rr = c.result();
            c.reset();
            slot.publish(rr);
        });

    const bool content_ok = file_content_is(path, "abXYZf");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 3)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool file_access_query_reports_open_mode() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;

    File read_file = std::move(File::open(path).value());
    if (read_file.access() != FileAccess::read_only)
        return false;

    File rw_file = std::move(File::open(path, writable_mode()).value());
    if (rw_file.access() != FileAccess::read_write)
        return false;

    FileOpen wo_mode;
    wo_mode.access = FileAccess::write_only;
    File wo_file = std::move(File::open(path, wo_mode).value());
    if (wo_file.access() != FileAccess::write_only)
        return false;

    File moved = std::move(rw_file);
    if (moved.access() != FileAccess::read_write)
        return false;

    const bool closed =
        read_file.close().has_value() && moved.close().has_value() && wo_file.close().has_value();
    ::unlink(path.c_str());
    return closed;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"write_replaces_bytes_at_offset", write_replaces_bytes_at_offset},
        {"write_can_extend_file", write_can_extend_file},
        {"write_empty_buffer_returns_zero_without_operation",
         write_empty_buffer_returns_zero_without_operation},
        {"write_after_close_reports_invalid_state", write_after_close_reports_invalid_state},
        {"write_with_read_only_access_fails", write_with_read_only_access_fails},
        {"write_result_reports_bytes_written", write_result_reports_bytes_written},
        {"write_keeps_resource_and_buffer_valid_until_terminal",
         write_keeps_resource_and_buffer_valid_until_terminal},
        {"file_access_query_reports_open_mode", file_access_query_reports_open_mode},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu file write tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
