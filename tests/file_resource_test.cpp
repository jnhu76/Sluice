#include <sluice/file_resource.hpp>

#include <cstdio>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;

std::string make_temp_path() {
    char path[] = "/tmp/sluice_file_resource_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    return path;
}

bool open_exposes_live_descriptor_and_close_releases_it() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    if (!file.is_open())
        return false;
    if (::fcntl(file.native_handle(), F_GETFD) < 0)
        return false;
    if (!file.close().has_value())
        return false;
    if (file.is_open())
        return false;
    if (::fcntl(file.native_handle(), F_GETFD) == 0)
        return false;
    return file.close().has_value();
}

bool moved_file_owns_no_descriptor() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    File target = std::move(file);
    if (file.is_open())
        return false;
    if (file.native_handle() >= 0)
        return false;
    if (::fcntl(target.native_handle(), F_GETFD) < 0)
        return false;
    return target.close().has_value();
}

bool open_missing_path_reports_error() {
    auto opened = File::open("/tmp/sluice_file_resource_missing_entry");
    if (opened.has_value())
        return false;
    return opened.error().os_errno == ENOENT;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"open_exposes_live_descriptor_and_close_releases_it",
         open_exposes_live_descriptor_and_close_releases_it},
        {"moved_file_owns_no_descriptor", moved_file_owns_no_descriptor},
        {"open_missing_path_reports_error", open_missing_path_reports_error},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu file resource tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
