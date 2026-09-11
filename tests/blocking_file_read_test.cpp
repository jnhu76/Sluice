#include <sluice/blocking/file.hpp>

#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;
using sluice::blocking::read_at;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_blocking_file_read_XXXXXX";
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

bool read_returns_bytes_at_offset() {
    const std::string path = make_temp_file("hello world");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    std::vector<std::byte> dst(5);
    auto result = read_at(file, 6, dst);

    if (!result.has_value())
        return false;
    if (result.value() != 5)
        return false;
    if (std::memcmp(dst.data(), "world", 5) != 0)
        return false;
    return file.close().has_value();
}

bool read_past_end_returns_zero() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    std::vector<std::byte> dst(4);
    auto result = read_at(file, 100, dst);

    if (!result.has_value())
        return false;
    if (result.value() != 0)
        return false;
    return file.close().has_value();
}

bool read_empty_buffer_returns_zero_without_operation() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    auto result = read_at(file, 0, std::span<std::byte>{});

    if (!result.has_value())
        return false;
    if (result.value() != 0)
        return false;
    return file.close().has_value();
}

bool read_after_close_reports_invalid_state() {
    const std::string path = make_temp_file("abcd");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    if (!file.close().has_value())
        return false;

    std::vector<std::byte> dst(4);
    auto result = read_at(file, 0, dst);

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_state)
        return false;
    return true;
}

bool read_with_illegal_offset_reports_invalid_argument() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    std::vector<std::byte> dst(1);
    auto result = read_at(file, std::numeric_limits<std::uint64_t>::max(), dst);

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
        {"read_returns_bytes_at_offset", read_returns_bytes_at_offset},
        {"read_past_end_returns_zero", read_past_end_returns_zero},
        {"read_empty_buffer_returns_zero_without_operation",
         read_empty_buffer_returns_zero_without_operation},
        {"read_after_close_reports_invalid_state", read_after_close_reports_invalid_state},
        {"read_with_illegal_offset_reports_invalid_argument",
         read_with_illegal_offset_reports_invalid_argument},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu blocking file read tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
