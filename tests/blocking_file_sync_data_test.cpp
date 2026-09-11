#include <sluice/blocking/file.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
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
using sluice::blocking::sync_data;
using sluice::blocking::write_at;

FileOpen writable_mode() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_blocking_file_sync_data_XXXXXX";
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
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string src_str = "XYZ";
    auto wr = write_at(file, 0, as_bytes(src_str));
    if (!wr.has_value() || wr.value() != 3)
        return false;

    auto result = sync_data(file);

    const bool content_ok = file_content_is(path, "XYZ");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool sync_data_after_close_reports_invalid_state() {
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

    auto result = sync_data(file);

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_state)
        return false;
    return true;
}

bool sync_data_on_read_only_file_succeeds() {
    const std::string path = make_temp_file("keep");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto result = sync_data(file);

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
    auto opened = File::open(path, mode);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto result = sync_data(file);

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    return file.close().has_value();
}

bool sync_data_on_device_reports_backend_error() {
    auto opened = File::open("/dev/null");
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto result = sync_data(file);

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
        {"sync_data_after_close_reports_invalid_state", sync_data_after_close_reports_invalid_state},
        {"sync_data_on_read_only_file_succeeds", sync_data_on_read_only_file_succeeds},
        {"sync_data_on_write_only_file_succeeds", sync_data_on_write_only_file_succeeds},
        {"sync_data_on_device_reports_backend_error", sync_data_on_device_reports_backend_error},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu blocking file sync data tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
