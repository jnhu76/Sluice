// Direct exact/all composition as real-file integration: the unscripted full
// and EOF paths, the placement of a positional composition, and the
// shared-cursor composition.

#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::blocking::CompositionEnd;
using sluice::blocking::EffectCertainty;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_direct_composition_XXXXXX";
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

bool file_content_is(const std::string& path, const std::string& expected) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    std::vector<char> buffer(expected.size() + 1, '\0');
    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
    ::close(fd);
    return n == static_cast<ssize_t>(expected.size()) &&
           std::string(buffer.data(), expected.size()) == expected;
}

bool read_exact_reads_the_whole_request() {
    const std::string path = make_temp_file("abcdefgh");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_only, file_holder))
        return false;
    File& file = *file_holder;

    std::vector<std::byte> dst(8, std::byte{0});
    auto composed = sluice::blocking::read_exact_at(file, 0, dst);
    ::unlink(path.c_str());
    return composed.has_value() && composed.value().complete() &&
           composed.value().confirmed_bytes == 8 &&
           composed.value().remaining == EffectCertainty::accounted &&
           dst[0] == std::byte{'a'} && dst[7] == std::byte{'h'} &&
           file.close().has_value();
}

bool read_exact_reports_eof_before_full_with_the_prefix() {
    const std::string path = make_temp_file("abcde");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_only, file_holder))
        return false;
    File& file = *file_holder;

    std::vector<std::byte> dst(16, std::byte{0});
    auto composed = sluice::blocking::read_exact_at(file, 2, dst);
    ::unlink(path.c_str());
    return composed.has_value() && composed.value().end == CompositionEnd::eof_before_full &&
           composed.value().confirmed_bytes == 3 &&
           composed.value().remaining == EffectCertainty::accounted &&
           dst[0] == std::byte{'c'} && file.close().has_value();
}

bool write_all_writes_the_whole_request() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_write, file_holder))
        return false;
    File& file = *file_holder;

    const std::string payload = "sluice direct composition";
    const std::span<const std::byte> src(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    auto composed = sluice::blocking::write_all(file, src);
    const bool ok = composed.has_value() && composed.value().complete() &&
                    composed.value().confirmed_bytes == payload.size() &&
                    composed.value().remaining == EffectCertainty::accounted &&
                    file.close().has_value() && file_content_is(path, payload);
    ::unlink(path.c_str());
    return ok;
}

bool positional_composition_places_bytes_and_leaves_the_cursor() {
    const std::string path = make_temp_file("0123456789");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_write, file_holder))
        return false;
    File& file = *file_holder;

    const std::string payload = "XY";
    const std::span<const std::byte> src(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    std::vector<std::byte> moved(2, std::byte{0});
    auto advanced = sluice::blocking::read(file, moved); // cursor -> 2
    auto placed = sluice::blocking::write_all_at(file, 5, src);
    std::vector<std::byte> next(2, std::byte{0});
    auto read_back = sluice::blocking::read(file, next); // reads at 2

    const bool ok = advanced.has_value() && advanced.value() == 2 && placed.has_value() &&
                    placed.value().complete() && read_back.has_value() &&
                    read_back.value() == 2 && next[0] == std::byte{'2'} &&
                    file.close().has_value() && file_content_is(path, "01234XY789");
    ::unlink(path.c_str());
    return ok;
}

bool shared_cursor_composition_advances_the_cursor() {
    const std::string path = make_temp_file("abcdefgh");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_write, file_holder))
        return false;
    File& file = *file_holder;

    std::vector<std::byte> first(3, std::byte{0});
    auto head = sluice::blocking::read_exact(file, first);
    std::vector<std::byte> second(5, std::byte{0});
    auto tail = sluice::blocking::read_exact(file, second);

    ::unlink(path.c_str());
    return head.has_value() && head.value().complete() && head.value().confirmed_bytes == 3 &&
           tail.has_value() && tail.value().complete() && tail.value().confirmed_bytes == 5 &&
           first[0] == std::byte{'a'} && second[0] == std::byte{'d'} &&
           file.close().has_value();
}

bool empty_request_completes_without_io() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_write, file_holder))
        return false;
    File& file = *file_holder;

    std::span<std::byte> empty_dst;
    std::span<const std::byte> empty_src;
    auto read = sluice::blocking::read_exact_at(file, 0, empty_dst);
    auto write = sluice::blocking::write_all(file, empty_src);
    ::unlink(path.c_str());
    return read.has_value() && read.value().complete() && read.value().confirmed_bytes == 0 &&
           write.has_value() && write.value().complete() && file.close().has_value();
}

bool composition_rejections_follow_the_shared_precedence() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_only, file_holder))
        return false;
    File& file = *file_holder;

    std::vector<std::byte> buffer(4, std::byte{0});
    auto illegal = sluice::blocking::write_all_at(file, 0, buffer);
    auto out_of_range =
        sluice::blocking::read_exact_at(file, std::numeric_limits<std::uint64_t>::max(), buffer);
    if (!file.close().has_value())
        return false;
    auto closed = sluice::blocking::read_exact_at(file, 0, buffer);
    ::unlink(path.c_str());

    return !illegal.has_value() && illegal.error().code == IoError::Code::invalid_argument &&
           !out_of_range.has_value() &&
           out_of_range.error().code == IoError::Code::invalid_argument && !closed.has_value() &&
           closed.error().code == IoError::Code::invalid_state;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"read_exact_reads_the_whole_request", read_exact_reads_the_whole_request},
        {"read_exact_reports_eof_before_full_with_the_prefix",
         read_exact_reports_eof_before_full_with_the_prefix},
        {"write_all_writes_the_whole_request", write_all_writes_the_whole_request},
        {"positional_composition_places_bytes_and_leaves_the_cursor",
         positional_composition_places_bytes_and_leaves_the_cursor},
        {"shared_cursor_composition_advances_the_cursor",
         shared_cursor_composition_advances_the_cursor},
        {"empty_request_completes_without_io", empty_request_completes_without_io},
        {"composition_rejections_follow_the_shared_precedence",
         composition_rejections_follow_the_shared_precedence},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu direct composition integration tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
