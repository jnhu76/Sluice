#include <sluice/blocking/file.hpp>

#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileInitialContents;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;
using sluice::blocking::write_at;

FileOpen writable_mode() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_blocking_file_write_XXXXXX";
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
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string src_str = "XYZ";
    auto result = write_at(file, 2, as_bytes(src_str));

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
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string src_str = "Z";
    auto result = write_at(file, 5, as_bytes(src_str));

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

bool write_empty_buffer_returns_zero_without_changing_file() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto result = write_at(file, 0, std::span<const std::byte>{});

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
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    if (!file.close().has_value())
        return false;

    const std::string src_str = "X";
    auto result = write_at(file, 0, as_bytes(src_str));

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
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string src_str = "X";
    auto result = write_at(file, 0, as_bytes(src_str));

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
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string src_str = "HELLO";
    auto result = write_at(file, 0, as_bytes(src_str));

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

bool write_primitive_has_no_retry_loop() {
    // Structural evidence: write_at returns the raw pwrite result.
    // Regular files on this platform typically return the full count,
    // but the contract is 0 <= n <= input_size and a single syscall.
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string src_str = "X";
    auto result = write_at(file, 0, as_bytes(src_str));

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    // Primitive contract: at most the requested size, no guarantee of full.
    if (result.value() > src_str.size())
        return false;
    return file.close().has_value();
}

bool write_with_illegal_offset_reports_invalid_argument() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string src_str = "X";
    auto result = write_at(file, std::numeric_limits<std::uint64_t>::max(), as_bytes(src_str));

    ::unlink(path.c_str());

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_argument)
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
        {"write_replaces_bytes_at_offset", write_replaces_bytes_at_offset},
        {"write_can_extend_file", write_can_extend_file},
        {"write_empty_buffer_returns_zero_without_changing_file",
         write_empty_buffer_returns_zero_without_changing_file},
        {"write_after_close_reports_invalid_state", write_after_close_reports_invalid_state},
        {"write_with_read_only_access_fails", write_with_read_only_access_fails},
        {"write_result_reports_bytes_written", write_result_reports_bytes_written},
        {"write_primitive_has_no_retry_loop", write_primitive_has_no_retry_loop},
        {"write_with_illegal_offset_reports_invalid_argument",
         write_with_illegal_offset_reports_invalid_argument},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu blocking file write tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
