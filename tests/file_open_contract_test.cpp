#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstring>
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

std::string reserve_temp_path() {
    char path[] = "/tmp/sluice_file_open_contract_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    ::unlink(path);
    return path;
}

bool raw_write(const std::string& path, const std::string& content) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    const ssize_t n = ::write(fd, content.data(), content.size());
    ::close(fd);
    return n == static_cast<ssize_t>(content.size());
}

bool raw_content_is(const std::string& path, const std::string& expected) {
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

bool raw_exists(const std::string& path) {
    struct ::stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

bool raw_size_is(const std::string& path, std::size_t expected) {
    struct ::stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return false;
    return static_cast<std::size_t>(st.st_size) == expected;
}

struct OpenCase {
    const char* name;
    FileAccess access;
    FileExistence existence;
    FileInitialContents contents;
    bool preexisting;
    bool expect_ok;
    bool expect_invalid_argument;
    bool expect_exists_after;
    const char* expect_content;
};

bool run_case(const OpenCase& c) {
    const std::string path = reserve_temp_path();
    if (path.empty())
        return false;
    if (c.preexisting && !raw_write(path, "keep"))
        return false;

    FileOpen mode;
    mode.access = c.access;
    mode.existence = c.existence;
    mode.contents = c.contents;
    auto opened = File::open(path, mode);

    bool ok = true;
    if (opened.has_value() != c.expect_ok)
        ok = false;
    if (ok && !opened.has_value() && c.expect_invalid_argument &&
        opened.error().code != IoError::Code::invalid_argument)
        ok = false;
    if (ok && !raw_exists(path) != !c.expect_exists_after)
        ok = false;
    if (ok && c.expect_exists_after) {
        if (!raw_content_is(path, c.expect_content))
            ok = false;
        if (std::strcmp(c.expect_content, "") == 0 && !raw_size_is(path, 0))
            ok = false;
    }
    if (ok && opened.has_value())
        (void)opened.value().close();
    ::unlink(path.c_str());
    return ok;
}

const OpenCase cases[] = {
    // read_only + preserve
    {"read_only_preserve_open_existing", FileAccess::read_only, FileExistence::open_existing,
     FileInitialContents::preserve, true, true, false, true, "keep"},
    {"read_only_preserve_open_existing_missing", FileAccess::read_only,
     FileExistence::open_existing, FileInitialContents::preserve, false, false, false, false, ""},
    {"read_only_preserve_create_if_missing", FileAccess::read_only,
     FileExistence::create_if_missing, FileInitialContents::preserve, true, true, false, true,
     "keep"},
    {"read_only_preserve_create_if_missing_missing", FileAccess::read_only,
     FileExistence::create_if_missing, FileInitialContents::preserve, false, true, false, true,
     ""},
    {"read_only_preserve_create_new", FileAccess::read_only, FileExistence::create_new,
     FileInitialContents::preserve, true, false, false, true, "keep"},
    {"read_only_preserve_create_new_missing", FileAccess::read_only, FileExistence::create_new,
     FileInitialContents::preserve, false, true, false, true, ""},
    // read_only + truncate: ILLEGAL for every existence, no destructive side effect
    {"read_only_truncate_open_existing", FileAccess::read_only, FileExistence::open_existing,
     FileInitialContents::truncate, true, false, true, true, "keep"},
    {"read_only_truncate_open_existing_missing", FileAccess::read_only,
     FileExistence::open_existing, FileInitialContents::truncate, false, false, true, false, ""},
    {"read_only_truncate_create_if_missing", FileAccess::read_only,
     FileExistence::create_if_missing, FileInitialContents::truncate, true, false, true, true,
     "keep"},
    {"read_only_truncate_create_if_missing_missing", FileAccess::read_only,
     FileExistence::create_if_missing, FileInitialContents::truncate, false, false, true, false,
     ""},
    {"read_only_truncate_create_new", FileAccess::read_only, FileExistence::create_new,
     FileInitialContents::truncate, true, false, true, true, "keep"},
    {"read_only_truncate_create_new_missing", FileAccess::read_only, FileExistence::create_new,
     FileInitialContents::truncate, false, false, true, false, ""},
    // write_only + preserve
    {"write_only_preserve_open_existing", FileAccess::write_only, FileExistence::open_existing,
     FileInitialContents::preserve, true, true, false, true, "keep"},
    {"write_only_preserve_open_existing_missing", FileAccess::write_only,
     FileExistence::open_existing, FileInitialContents::preserve, false, false, false, false, ""},
    {"write_only_preserve_create_if_missing", FileAccess::write_only,
     FileExistence::create_if_missing, FileInitialContents::preserve, true, true, false, true,
     "keep"},
    {"write_only_preserve_create_if_missing_missing", FileAccess::write_only,
     FileExistence::create_if_missing, FileInitialContents::preserve, false, true, false, true,
     ""},
    {"write_only_preserve_create_new", FileAccess::write_only, FileExistence::create_new,
     FileInitialContents::preserve, true, false, false, true, "keep"},
    {"write_only_preserve_create_new_missing", FileAccess::write_only, FileExistence::create_new,
     FileInitialContents::preserve, false, true, false, true, ""},
    // write_only + truncate
    {"write_only_truncate_open_existing", FileAccess::write_only, FileExistence::open_existing,
     FileInitialContents::truncate, true, true, false, true, ""},
    {"write_only_truncate_open_existing_missing", FileAccess::write_only,
     FileExistence::open_existing, FileInitialContents::truncate, false, false, false, false, ""},
    {"write_only_truncate_create_if_missing", FileAccess::write_only,
     FileExistence::create_if_missing, FileInitialContents::truncate, true, true, false, true, ""},
    {"write_only_truncate_create_if_missing_missing", FileAccess::write_only,
     FileExistence::create_if_missing, FileInitialContents::truncate, false, true, false, true,
     ""},
    {"write_only_truncate_create_new", FileAccess::write_only, FileExistence::create_new,
     FileInitialContents::truncate, true, false, false, true, "keep"},
    {"write_only_truncate_create_new_missing", FileAccess::write_only, FileExistence::create_new,
     FileInitialContents::truncate, false, true, false, true, ""},
    // read_write + preserve
    {"read_write_preserve_open_existing", FileAccess::read_write, FileExistence::open_existing,
     FileInitialContents::preserve, true, true, false, true, "keep"},
    {"read_write_preserve_open_existing_missing", FileAccess::read_write,
     FileExistence::open_existing, FileInitialContents::preserve, false, false, false, false, ""},
    {"read_write_preserve_create_if_missing", FileAccess::read_write,
     FileExistence::create_if_missing, FileInitialContents::preserve, true, true, false, true,
     "keep"},
    {"read_write_preserve_create_if_missing_missing", FileAccess::read_write,
     FileExistence::create_if_missing, FileInitialContents::preserve, false, true, false, true,
     ""},
    {"read_write_preserve_create_new", FileAccess::read_write, FileExistence::create_new,
     FileInitialContents::preserve, true, false, false, true, "keep"},
    {"read_write_preserve_create_new_missing", FileAccess::read_write, FileExistence::create_new,
     FileInitialContents::preserve, false, true, false, true, ""},
    // read_write + truncate
    {"read_write_truncate_open_existing", FileAccess::read_write, FileExistence::open_existing,
     FileInitialContents::truncate, true, true, false, true, ""},
    {"read_write_truncate_open_existing_missing", FileAccess::read_write,
     FileExistence::open_existing, FileInitialContents::truncate, false, false, false, false, ""},
    {"read_write_truncate_create_if_missing", FileAccess::read_write,
     FileExistence::create_if_missing, FileInitialContents::truncate, true, true, false, true, ""},
    {"read_write_truncate_create_if_missing_missing", FileAccess::read_write,
     FileExistence::create_if_missing, FileInitialContents::truncate, false, true, false, true,
     ""},
    {"read_write_truncate_create_new", FileAccess::read_write, FileExistence::create_new,
     FileInitialContents::truncate, true, false, false, true, "keep"},
    {"read_write_truncate_create_new_missing", FileAccess::read_write, FileExistence::create_new,
     FileInitialContents::truncate, false, true, false, true, ""},
};

} // namespace

int main() {
    for (const OpenCase& c : cases) {
        if (!run_case(c)) {
            std::fprintf(stderr, "FAIL: %s\n", c.name);
            return 1;
        }
    }
    std::printf("all %zu file open contract cases passed\n",
                sizeof(cases) / sizeof(cases[0]));
    return 0;
}
