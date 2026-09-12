#include <sluice/blocking/file.hpp>

#include <cerrno>
#include <cstdint>
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
using sluice::blocking::resize;
using sluice::blocking::size;
using sluice::blocking::write_at;

FileOpen writable_mode() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

FileOpen write_only_mode() {
    FileOpen mode;
    mode.access = FileAccess::write_only;
    return mode;
}

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_blocking_file_state_XXXXXX";
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

std::span<const std::byte> as_bytes(const std::string& src) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(src.data()), src.size());
}

bool file_reads_as(const File& file, std::uint64_t offset, std::size_t want,
                   const char* expected) {
    std::vector<char> buf(want, '\0');
    auto rd = read_at(file, offset,
                      std::span<std::byte>(reinterpret_cast<std::byte*>(buf.data()), buf.size()));
    if (!rd.has_value() || rd.value() != want)
        return false;
    return std::memcmp(buf.data(), expected, want) == 0;
}

bool size_on_empty_file_is_zero() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto result = size(file);

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 0)
        return false;
    return file.close().has_value();
}

bool size_after_write_reports_written_size() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string src_str = "abcde";
    auto wr = write_at(file, 0, as_bytes(src_str));
    if (!wr.has_value() || wr.value() != 5)
        return false;

    auto result = size(file);

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 5)
        return false;
    return file.close().has_value();
}

bool size_on_read_only_file_succeeds() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto result = size(file);

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 3)
        return false;
    return file.close().has_value();
}

bool size_on_write_only_file_succeeds() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path, write_only_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto result = size(file);

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 3)
        return false;
    return file.close().has_value();
}

bool size_after_close_reports_invalid_state() {
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

    auto result = size(file);

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_state)
        return false;
    return true;
}

bool resize_grow_zero_fills() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto wr = write_at(file, 0, as_bytes("abc"));
    if (!wr.has_value() || wr.value() != 3)
        return false;

    auto grown = resize(file, 10);
    auto sz = size(file);

    const bool zeros_ok = file_reads_as(file, 3, 7, "\0\0\0\0\0\0\0");

    ::unlink(path.c_str());

    if (!grown.has_value())
        return false;
    if (!sz.has_value() || sz.value() != 10)
        return false;
    if (!zeros_ok)
        return false;
    return file.close().has_value();
}

bool resize_shrink_truncates() {
    const std::string path = make_temp_file("abcdef");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto shrunk = resize(file, 3);
    auto sz = size(file);

    const bool content_ok = file_reads_as(file, 0, 3, "abc");

    ::unlink(path.c_str());

    if (!shrunk.has_value())
        return false;
    if (!sz.has_value() || sz.value() != 3)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool resize_to_zero_empties() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto emptied = resize(file, 0);

    auto sz = size(file);

    ::unlink(path.c_str());

    if (!emptied.has_value())
        return false;
    if (!sz.has_value() || sz.value() != 0)
        return false;
    return file.close().has_value();
}

bool resize_to_same_size_succeeds() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto same = resize(file, 3);

    const bool content_ok = file_reads_as(file, 0, 3, "abc");

    ::unlink(path.c_str());

    if (!same.has_value())
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool resize_on_read_only_reports_invalid_argument() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto result = resize(file, 5);

    const bool content_ok = file_reads_as(file, 0, 3, "abc");

    ::unlink(path.c_str());

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_argument)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool resize_after_close_reports_invalid_state() {
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

    auto result = resize(file, 3);

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_state)
        return false;
    return true;
}

bool resize_overflow_reports_invalid_argument() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto result = resize(file, std::numeric_limits<std::uint64_t>::max());

    ::unlink(path.c_str());

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_argument)
        return false;
    return file.close().has_value();
}

bool resize_on_device_reports_backend_error() {
    auto opened = File::open("/dev/null", writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto result = resize(file, 4);

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::backend_error)
        return false;
    if (result.error().os_errno != EINVAL)
        return false;
    return file.close().has_value();
}

bool resize_on_write_only_file_succeeds() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    auto opened = File::open(path, write_only_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto grown = resize(file, 2);

    if (!grown.has_value())
        return false;
    if (!file.close().has_value())
        return false;

    auto verify_opened = File::open(path);
    if (!verify_opened.has_value())
        return false;
    File verify = std::move(verify_opened).value();

    auto sz = size(verify);

    ::unlink(path.c_str());

    if (!sz.has_value() || sz.value() != 2)
        return false;
    return verify.close().has_value();
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"size_on_empty_file_is_zero", size_on_empty_file_is_zero},
        {"size_after_write_reports_written_size", size_after_write_reports_written_size},
        {"size_on_read_only_file_succeeds", size_on_read_only_file_succeeds},
        {"size_on_write_only_file_succeeds", size_on_write_only_file_succeeds},
        {"size_after_close_reports_invalid_state", size_after_close_reports_invalid_state},
        {"resize_grow_zero_fills", resize_grow_zero_fills},
        {"resize_shrink_truncates", resize_shrink_truncates},
        {"resize_to_zero_empties", resize_to_zero_empties},
        {"resize_to_same_size_succeeds", resize_to_same_size_succeeds},
        {"resize_on_read_only_reports_invalid_argument", resize_on_read_only_reports_invalid_argument},
        {"resize_after_close_reports_invalid_state", resize_after_close_reports_invalid_state},
        {"resize_overflow_reports_invalid_argument", resize_overflow_reports_invalid_argument},
        {"resize_on_device_reports_backend_error", resize_on_device_reports_backend_error},
        {"resize_on_write_only_file_succeeds", resize_on_write_only_file_succeeds},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu blocking file state tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
