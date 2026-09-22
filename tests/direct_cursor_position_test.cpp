#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_cursor_position_XXXXXX";
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

bool open_path(const std::string& path, FileAccess access, std::optional<File>& file) {
    FileOpen mode;
    mode.existence = sluice::FileExistence::open_existing;
    mode.access = access;
    auto opened = File::open(path, mode);
    if (!opened.has_value())
        return false;
    file = std::move(opened).value();
    return true;
}

bool shared_cursor_write_advances_and_positional_write_does_not() {
    const std::string path = make_temp_file("0123456789");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_write, file_holder))
        return false;
    File& file = *file_holder;

    const std::vector<std::byte> skip(3, std::byte{'x'});
    const std::vector<std::byte> placed(2, std::byte{'A'});
    const std::vector<std::byte> far(2, std::byte{'B'});
    std::vector<std::byte> rest(4, std::byte{0});

    auto advanced = sluice::blocking::write(file, skip);
    auto positioned = sluice::blocking::write_at(file, 8, far);
    auto written = sluice::blocking::write(file, placed);
    auto read_back = sluice::blocking::read(file, rest);

    ::unlink(path.c_str());
    return advanced.has_value() && advanced.value() == 3 && positioned.has_value() &&
           positioned.value() == 2 && written.has_value() && written.value() == 2 &&
           read_back.has_value() && read_back.value() == 4 && rest[0] == std::byte{'5'} &&
           file.close().has_value();
}

bool shared_cursor_is_shared_through_native_duplication() {
    const std::string path = make_temp_file("abcdefgh");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_only, file_holder))
        return false;
    File& file = *file_holder;
    const int duplicate = ::dup(file.native_handle());
    if (duplicate < 0)
        return false;

    std::vector<std::byte> two(2, std::byte{0});
    auto first = sluice::blocking::read(file, two);
    std::byte foreign[2] = {};
    const ssize_t n = ::read(duplicate, foreign, sizeof(foreign));

    ::close(duplicate);
    ::unlink(path.c_str());
    return first.has_value() && first.value() == 2 && n == 2 && two[0] == std::byte{'a'} &&
           two[1] == std::byte{'b'} && foreign[0] == std::byte{'c'} &&
           foreign[1] == std::byte{'d'} &&
           file.close().has_value();
}

bool independent_opens_have_independent_cursors() {
    const std::string path = make_temp_file("abcdefgh");
    if (path.empty())
        return false;
    std::optional<File> first_holder;
    std::optional<File> second_holder;
    if (!open_path(path, FileAccess::read_only, first_holder) ||
        !open_path(path, FileAccess::read_only, second_holder))
        return false;
    File& first = *first_holder;
    File& second = *second_holder;

    std::vector<std::byte> three(3, std::byte{0});
    std::vector<std::byte> one(1, std::byte{0});
    auto moved = sluice::blocking::read(first, three);
    auto untouched = sluice::blocking::read(second, one);

    ::unlink(path.c_str());
    return moved.has_value() && moved.value() == 3 && untouched.has_value() &&
           untouched.value() == 1 && one[0] == std::byte{'a'} && first.close().has_value() &&
           second.close().has_value();
}

bool resize_does_not_move_the_shared_cursor() {
    const std::string path = make_temp_file("abcdefgh");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_write, file_holder))
        return false;
    File& file = *file_holder;

    std::vector<std::byte> three(3, std::byte{0});
    auto moved = sluice::blocking::read(file, three);
    auto grown = sluice::blocking::resize(file, 16);
    auto shrunk = sluice::blocking::resize(file, 12);
    std::vector<std::byte> next(2, std::byte{0});
    auto read_back = sluice::blocking::read(file, next);

    ::unlink(path.c_str());
    return moved.has_value() && moved.value() == 3 && grown.has_value() && shrunk.has_value() &&
           read_back.has_value() && read_back.value() == 2 && next[0] == std::byte{'d'} &&
           next[1] == std::byte{'e'} && file.close().has_value();
}

bool zero_length_shared_cursor_call_leaves_the_cursor() {
    const std::string path = make_temp_file("abcdefgh");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_write, file_holder))
        return false;
    File& file = *file_holder;

    std::vector<std::byte> two(2, std::byte{0});
    std::span<std::byte> empty;
    auto moved = sluice::blocking::read(file, two);
    auto noop_read = sluice::blocking::read(file, empty);
    auto noop_write = sluice::blocking::write(file, empty);
    std::vector<std::byte> next(1, std::byte{0});
    auto read_back = sluice::blocking::read(file, next);

    ::unlink(path.c_str());
    return moved.has_value() && moved.value() == 2 && noop_read.has_value() &&
           noop_read.value() == 0 && noop_write.has_value() && noop_write.value() == 0 &&
           read_back.has_value() && read_back.value() == 1 && next[0] == std::byte{'c'} &&
           file.close().has_value();
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"shared_cursor_write_advances_and_positional_write_does_not",
         shared_cursor_write_advances_and_positional_write_does_not},
        {"shared_cursor_is_shared_through_native_duplication",
         shared_cursor_is_shared_through_native_duplication},
        {"independent_opens_have_independent_cursors",
         independent_opens_have_independent_cursors},
        {"resize_does_not_move_the_shared_cursor", resize_does_not_move_the_shared_cursor},
        {"zero_length_shared_cursor_call_leaves_the_cursor",
         zero_length_shared_cursor_call_leaves_the_cursor},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu direct cursor-position tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
