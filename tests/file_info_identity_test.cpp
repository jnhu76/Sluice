// Minimal metadata and same-file identity (SEM-07) on the direct path: one real
// `fstat` integration, the `size` projection of the same observation, and the
// value-level availability rules that Linux cannot produce on demand.

#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileIdentity;
using sluice::FileInfo;
using sluice::FileKind;
using sluice::FileOpen;
using sluice::IdentityMatch;
using sluice::IoError;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_file_info_XXXXXX";
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

bool file_info_reports_regular_kind_size_and_identity() {
    const std::string path = make_temp_file("hello");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_only, file_holder))
        return false;
    File& file = *file_holder;

    auto info = sluice::blocking::file_info(file);
    ::unlink(path.c_str());
    if (!info.has_value())
        return false;
    const FileInfo& value = info.value();
    if (value.kind != FileKind::regular)
        return false;
    if (value.size != 5)
        return false;
    if (!value.identity.has_value())
        return false;
    return file.close().has_value();
}

// `size` is the projection of its own `file_info` observation rather than a
// second metadata rule, so the two agree while no external mutation falls
// between them. Separate calls are not a transaction (SEM-07).
bool size_projects_file_info() {
    const std::string path = make_temp_file("abcd");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_write, file_holder))
        return false;
    File& file = *file_holder;

    auto info = sluice::blocking::file_info(file);
    auto sz = sluice::blocking::size(file);
    bool ok = info.has_value() && sz.has_value();
    ok = ok && sz.value() == info.value().size;
    ok = ok && sz.value() == 4;
    ok = ok && sluice::blocking::resize(file, 9).has_value();
    auto grown_info = sluice::blocking::file_info(file);
    auto grown_size = sluice::blocking::size(file);
    ok = ok && grown_info.has_value() && grown_size.has_value();
    ok = ok && grown_size.value() == grown_info.value().size;
    ok = ok && grown_size.value() == 9;
    // A resize changes the observed size but not the identity.
    ok = ok && grown_info.value().identity == info.value().identity;
    ::unlink(path.c_str());
    ok = ok && file.close().has_value();
    return ok;
}

// Two independent opens of one file are the same file; different files are not.
bool identity_of_two_opens_matches_and_different_files_differ() {
    const std::string path_a = make_temp_file("same");
    const std::string path_b = make_temp_file("other");
    if (path_a.empty() || path_b.empty())
        return false;
    std::optional<File> first_holder;
    std::optional<File> second_holder;
    std::optional<File> other_holder;
    if (!open_path(path_a, FileAccess::read_only, first_holder) ||
        !open_path(path_a, FileAccess::read_only, second_holder) ||
        !open_path(path_b, FileAccess::read_only, other_holder)) {
        return false;
    }
    File& first = *first_holder;
    File& second = *second_holder;
    File& other = *other_holder;

    auto info_a = sluice::blocking::file_info(first);
    auto info_b = sluice::blocking::file_info(second);
    auto info_c = sluice::blocking::file_info(other);
    ::unlink(path_a.c_str());
    ::unlink(path_b.c_str());
    if (!info_a.has_value() || !info_b.has_value() || !info_c.has_value())
        return false;

    const bool ok = sluice::identity_match(info_a.value(), info_b.value()) == IdentityMatch::same &&
                    sluice::identity_match(info_a.value(), info_c.value()) ==
                        IdentityMatch::different &&
                    first.close().has_value() && second.close().has_value() &&
                    other.close().has_value();
    return ok;
}

// Availability is a value-level rule: Linux always supplies device/inode, so the
// unavailable outcome is exercised on the representation rather than by faking a
// kernel result.
bool identity_match_reports_unknown_when_a_side_is_unavailable() {
    FileInfo known;
    known.kind = FileKind::regular;
    known.size = 1;
    known.identity = FileIdentity{7, 11};
    FileInfo other_known;
    other_known.identity = FileIdentity{7, 12};
    FileInfo unavailable;
    FileInfo unavailable_too;

    return sluice::identity_match(known, known) == IdentityMatch::same &&
           sluice::identity_match(known, other_known) == IdentityMatch::different &&
           sluice::identity_match(known, unavailable) == IdentityMatch::unknown &&
           sluice::identity_match(unavailable, known) == IdentityMatch::unknown &&
           sluice::identity_match(unavailable, unavailable_too) == IdentityMatch::unknown;
}

// file_info is a state operation: a closed File is invalid_state, and the access
// mode does not matter.
bool file_info_on_closed_file_is_invalid_state() {
    const std::string path = make_temp_file("closed");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_only, file_holder))
        return false;
    File& file = *file_holder;
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto info = sluice::blocking::file_info(file);
    auto sz = sluice::blocking::size(file);
    return !info.has_value() && info.error().code == IoError::Code::invalid_state &&
           !sz.has_value() && sz.error().code == IoError::Code::invalid_state;
}

// A kind outside the ordinary-file domain is reported, not refused: v1 promises
// no data-I/O semantics for it, and this test only observes the report.
bool file_info_reports_other_kinds_without_promising_data_io() {
    std::optional<File> dir_holder;
    if (!open_path("/tmp", FileAccess::read_only, dir_holder))
        return false;
    File& dir = *dir_holder;
    auto info = sluice::blocking::file_info(dir);
    if (!info.has_value())
        return false;
    const bool ok = info.value().kind == FileKind::other && dir.close().has_value();
    return ok;
}

// SEM-07: neither file_info nor positional I/O moves the shared cursor.
bool metadata_and_positional_io_do_not_move_the_shared_cursor() {
    const std::string path = make_temp_file("abcdefgh");
    if (path.empty())
        return false;
    std::optional<File> file_holder;
    if (!open_path(path, FileAccess::read_write, file_holder))
        return false;
    File& file = *file_holder;

    std::vector<std::byte> two(2, std::byte{0});
    auto first = sluice::blocking::read(file, two);
    auto info = sluice::blocking::file_info(file);
    auto sz = sluice::blocking::size(file);
    std::vector<std::byte> at(2, std::byte{0});
    auto positioned = sluice::blocking::read_at(file, 6, at);
    std::vector<std::byte> next(2, std::byte{0});
    auto second = sluice::blocking::read(file, next);

    ::unlink(path.c_str());
    const bool ok = first.has_value() && first.value() == 2 && info.has_value() &&
                    sz.has_value() && positioned.has_value() && positioned.value() == 2 &&
                    second.has_value() && second.value() == 2 &&
                    // The cursor stayed at 2: the bytes read are "cd", not "gh".
                    next[0] == std::byte{'c'} && next[1] == std::byte{'d'} &&
                    file.close().has_value();
    return ok;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"file_info_reports_regular_kind_size_and_identity",
         file_info_reports_regular_kind_size_and_identity},
        {"size_projects_file_info", size_projects_file_info},
        {"identity_of_two_opens_matches_and_different_files_differ",
         identity_of_two_opens_matches_and_different_files_differ},
        {"identity_match_reports_unknown_when_a_side_is_unavailable",
         identity_match_reports_unknown_when_a_side_is_unavailable},
        {"file_info_on_closed_file_is_invalid_state", file_info_on_closed_file_is_invalid_state},
        {"file_info_reports_other_kinds_without_promising_data_io",
         file_info_reports_other_kinds_without_promising_data_io},
        {"metadata_and_positional_io_do_not_move_the_shared_cursor",
         metadata_and_positional_io_do_not_move_the_shared_cursor},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu direct metadata and identity tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
