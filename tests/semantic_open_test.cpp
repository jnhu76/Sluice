// SEM-02 open semantics: combination legality and the embedded-NUL rule, frozen
// as a decision table. The table is the oracle; the real open outcome is checked
// against it in both directions so neither can drift alone.
//
// The destructive half of the SEM-02 matrix (which combinations truncate or fail
// to create) is covered by the existing file_open_contract_test, which already
// runs all 36 existence x contents x access cases against real files.
#include <sluice/detail/file_semantics.hpp>
#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileInitialContents;
using sluice::FileOpen;
using sluice::IoError;
using sluice::detail::OpenVerdict;

constexpr FileAccess kAccesses[] = {FileAccess::read_only, FileAccess::write_only,
                                    FileAccess::read_write};
constexpr FileExistence kExistences[] = {FileExistence::open_existing,
                                         FileExistence::create_if_missing,
                                         FileExistence::create_new};
constexpr FileInitialContents kContents[] = {FileInitialContents::preserve,
                                             FileInitialContents::truncate};

// SEM-02: the only illegal combination is read-only access combined with
// truncation, and it is illegal for every existence choice.
bool legality_table_is_frozen() {
    for (FileAccess access : kAccesses) {
        for (FileExistence existence : kExistences) {
            for (FileInitialContents contents : kContents) {
                FileOpen mode;
                mode.access = access;
                mode.existence = existence;
                mode.contents = contents;
                const bool illegal =
                    access == FileAccess::read_only && contents == FileInitialContents::truncate;
                const OpenVerdict verdict = sluice::detail::precheck_open(mode, "probe");
                if (illegal && verdict != OpenVerdict::reject_read_only_truncate)
                    return false;
                if (!illegal && verdict != OpenVerdict::legal)
                    return false;
            }
        }
    }
    return true;
}

std::string reserve_temp_path() {
    char path[] = "/tmp/sluice_semantic_open_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    ::unlink(path);
    return path;
}

// The oracle's legality must equal the real open outcome's legality: an illegal
// combination returns invalid_argument for any existence, and a legal one never
// does (it may still fail for a filesystem reason, which is an operation result).
bool oracle_legality_matches_real_open() {
    const std::string path = reserve_temp_path();
    if (path.empty())
        return false;

    bool ok = true;
    for (FileAccess access : kAccesses) {
        for (FileExistence existence : kExistences) {
            for (FileInitialContents contents : kContents) {
                FileOpen mode;
                mode.access = access;
                mode.existence = existence;
                mode.contents = contents;
                const OpenVerdict verdict = sluice::detail::precheck_open(mode, path);
                auto opened = File::open(path, mode);

                const bool rejected_by_oracle = verdict != OpenVerdict::legal;
                const bool rejected_by_open =
                    !opened.has_value() && opened.error().code == IoError::Code::invalid_argument;
                if (rejected_by_oracle != rejected_by_open) {
                    std::fprintf(stderr,
                                 "oracle/open disagree: access=%d existence=%d contents=%d\n",
                                 static_cast<int>(access), static_cast<int>(existence),
                                 static_cast<int>(contents));
                    ok = false;
                }
                if (opened.has_value())
                    (void)opened.value().close();
                ::unlink(path.c_str());
            }
        }
    }
    ::unlink(path.c_str());
    return ok;
}

bool embedded_nul_is_rejected() {
    auto opened = File::open(std::string("a\0b", 3));
    if (opened.has_value())
        return false;
    const IoError e = opened.error();
    return e.code == IoError::Code::invalid_argument && e.os_errno == 0;
}

// The discriminating case for "rejected before native open": the truncated name
// denotes a real, openable file. If the path were passed to the native open, the
// name would be cut at the NUL and the open would succeed.
bool embedded_nul_is_rejected_before_native_open() {
    const std::string path = reserve_temp_path();
    if (path.empty())
        return false;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    ::close(fd);

    const std::string with_nul = path + std::string("\0suffix", 7);
    auto opened = File::open(with_nul);
    ::unlink(path.c_str());
    if (opened.has_value()) {
        (void)opened.value().close();
        return false;
    }
    return opened.error().code == IoError::Code::invalid_argument;
}

// A NUL that follows a valid, existing path must also be rejected rather than
// opening the prefix, and it must leave the prefix untouched.
bool embedded_nul_leaves_target_untouched() {
    const std::string path = reserve_temp_path();
    if (path.empty())
        return false;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    const ssize_t n = ::write(fd, "keep", 4);
    ::close(fd);
    if (n != 4) {
        ::unlink(path.c_str());
        return false;
    }

    FileOpen mode;
    mode.access = FileAccess::write_only;
    mode.contents = FileInitialContents::truncate;
    auto opened = File::open(path + std::string("\0", 1), mode);

    char buffer[8] = {};
    const int check = ::open(path.c_str(), O_RDONLY);
    ssize_t read_back = -1;
    if (check >= 0) {
        read_back = ::read(check, buffer, sizeof(buffer));
        ::close(check);
    }
    ::unlink(path.c_str());
    if (opened.has_value())
        return false;
    return read_back == 4 && std::memcmp(buffer, "keep", 4) == 0;
}

} // namespace

int main() {
    if (!legality_table_is_frozen()) {
        std::fprintf(stderr, "FAIL: legality_table_is_frozen\n");
        return 1;
    }
    if (!oracle_legality_matches_real_open()) {
        std::fprintf(stderr, "FAIL: oracle_legality_matches_real_open\n");
        return 1;
    }
    if (!embedded_nul_is_rejected()) {
        std::fprintf(stderr, "FAIL: embedded_nul_is_rejected\n");
        return 1;
    }
    if (!embedded_nul_is_rejected_before_native_open()) {
        std::fprintf(stderr, "FAIL: embedded_nul_is_rejected_before_native_open\n");
        return 1;
    }
    if (!embedded_nul_leaves_target_untouched()) {
        std::fprintf(stderr, "FAIL: embedded_nul_leaves_target_untouched\n");
        return 1;
    }
    std::printf("all 5 open semantic tests passed (18 combination table cases)\n");
    return 0;
}
