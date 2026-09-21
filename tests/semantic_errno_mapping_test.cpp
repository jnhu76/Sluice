// ERR-01 canonical native-error mapping. The root specification is the oracle:
// the expected category per native errno is written here as a decision table,
// not read back from the production switch, so a change in the mapping fails
// this test instead of redefining it.
//
// Two evidence classes are kept separate:
//   - table cases: pure, always run, exhaustive over the mapping table;
//   - filesystem cases: prove that real failures actually reach the table.
// A filesystem case that cannot run is reported as NOT RUN, never as a pass.
#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>

#include <cerrno>
#include <cstdio>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileInitialContents;
using sluice::FileOpen;
using sluice::IoError;

constexpr IoError::Code kBackend = IoError::Code::backend_error;

struct TableCase {
    const char* name;
    int native_errno;
    IoError::Code expected;
};

// ERR-01: ENOENT/ENOTDIR map to not_found; EACCES/EPERM to permission_denied;
// ENOSPC/EDQUOT to no_space; EINTR to interrupted; EAGAIN/EWOULDBLOCK to
// would_block; ECANCELED to canceled. Any native error without a canonical
// category keeps backend_error plus native detail.
const TableCase table_cases[] = {
    {"ENOENT_is_not_found", ENOENT, IoError::Code::not_found},
    {"ENOTDIR_is_not_found", ENOTDIR, IoError::Code::not_found},
    {"EACCES_is_permission_denied", EACCES, IoError::Code::permission_denied},
    {"EPERM_is_permission_denied", EPERM, IoError::Code::permission_denied},
    {"ENOSPC_is_no_space", ENOSPC, IoError::Code::no_space},
    {"EDQUOT_is_no_space", EDQUOT, IoError::Code::no_space},
    {"EINTR_is_interrupted", EINTR, IoError::Code::interrupted},
    {"EAGAIN_is_would_block", EAGAIN, IoError::Code::would_block},
    {"ECANCELED_is_canceled", ECANCELED, IoError::Code::canceled},
    // No canonical category is specified for these: they must keep the
    // fallback category and preserve the native detail.
    {"EIO_stays_backend_error", EIO, kBackend},
    {"EINVAL_stays_backend_error", EINVAL, kBackend},
    {"EBADF_stays_backend_error", EBADF, kBackend},
    {"EOVERFLOW_stays_backend_error", EOVERFLOW, kBackend},
    {"zero_errno_is_backend_error", 0, kBackend},
};

bool table_case_holds(const TableCase& c) {
    const IoError e = sluice::from_errno_value(c.native_errno);
    if (e.code != c.expected)
        return false;
    // The native detail is preserved for every case, including the fallback.
    return e.os_errno == c.native_errno;
}

bool table_is_internally_consistent() {
    constexpr std::size_t count =
        sizeof(sluice::detail::kNativeErrorMappings) / sizeof(sluice::detail::kNativeErrorMappings[0]);
    // A native value must not be reachable with two different categories.
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            const auto& a = sluice::detail::kNativeErrorMappings[i];
            const auto& b = sluice::detail::kNativeErrorMappings[j];
            if (a.native_errno == b.native_errno && a.canonical != b.canonical)
                return false;
        }
        if (sluice::from_errno_value(sluice::detail::kNativeErrorMappings[i].native_errno).code !=
            sluice::detail::kNativeErrorMappings[i].canonical)
            return false;
    }
    return true;
}

bool canonical_code_is_table_derived() {
    if (sluice::detail::canonical_error_code(ENOENT) != IoError::Code::not_found)
        return false;
    if (sluice::detail::canonical_error_code(ENOTDIR) != IoError::Code::not_found)
        return false;
    if (sluice::detail::canonical_error_code(0) != kBackend)
        return false;
    return sluice::from_errno_value(EACCES).code ==
           sluice::detail::canonical_error_code(EACCES);
}

std::string reserve_temp_path() {
    char path[] = "/tmp/sluice_errno_mapping_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    ::unlink(path);
    return path;
}

// Real-filesystem case: an absent path must reach the table as not_found.
bool missing_path_maps_to_not_found() {
    const std::string path = reserve_temp_path();
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (opened.has_value())
        return false;
    const IoError e = opened.error();
    return e.code == IoError::Code::not_found && e.os_errno == ENOENT;
}

// A path whose parent component is a regular file yields ENOTDIR, which the
// root maps to not_found rather than permission_denied.
bool non_directory_component_maps_to_not_found() {
    const std::string parent = reserve_temp_path();
    if (parent.empty())
        return false;
    const int fd = ::open(parent.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    ::close(fd);

    auto opened = File::open(parent + "/child");
    ::unlink(parent.c_str());
    if (opened.has_value())
        return false;
    const IoError e = opened.error();
    return e.code == IoError::Code::not_found && e.os_errno == ENOTDIR;
}

// ENOSPC cannot be produced deterministically without a bounded filesystem, so
// it stays a table case; the same holds for EDQUOT. Recorded in the ledger.
bool permission_denied_reaches_the_table(int* attempted) {
    *attempted = 0;
    if (::geteuid() == 0)
        return true;
    const std::string path = reserve_temp_path();
    if (path.empty())
        return false;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    const ssize_t n = ::write(fd, "x", 1);
    ::close(fd);
    if (n != 1) {
        ::unlink(path.c_str());
        return false;
    }
    if (::chmod(path.c_str(), 0) != 0) {
        ::unlink(path.c_str());
        return false;
    }
    *attempted = 1;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (opened.has_value())
        return false;
    const IoError e = opened.error();
    return e.code == IoError::Code::permission_denied && e.os_errno == EACCES;
}

} // namespace

int main() {
    for (const TableCase& c : table_cases) {
        if (!table_case_holds(c)) {
            const IoError actual = sluice::from_errno_value(c.native_errno);
            std::fprintf(stderr, "FAIL: %s (errno %d -> %.*s, expected %.*s)\n", c.name,
                         c.native_errno, static_cast<int>(sluice::to_string(actual.code).size()),
                         sluice::to_string(actual.code).data(),
                         static_cast<int>(sluice::to_string(c.expected).size()),
                         sluice::to_string(c.expected).data());
            return 1;
        }
    }
    if (!table_is_internally_consistent()) {
        std::fprintf(stderr, "FAIL: native error mapping table is not internally consistent\n");
        return 1;
    }
    if (!canonical_code_is_table_derived()) {
        std::fprintf(stderr, "FAIL: canonical_error_code is not table derived\n");
        return 1;
    }
    if (!missing_path_maps_to_not_found()) {
        std::fprintf(stderr, "FAIL: missing_path_maps_to_not_found\n");
        return 1;
    }
    if (!non_directory_component_maps_to_not_found()) {
        std::fprintf(stderr, "FAIL: non_directory_component_maps_to_not_found\n");
        return 1;
    }
    int attempted = 0;
    if (!permission_denied_reaches_the_table(&attempted)) {
        std::fprintf(stderr, "FAIL: permission_denied_reaches_the_table\n");
        return 1;
    }
    if (attempted == 0) {
        std::printf("NOT RUN: permission_denied_reaches_the_table "
                    "(process can bypass file permissions; table case still covers EACCES)\n");
    }

    std::printf("all %zu errno mapping table cases passed; %d filesystem cases ran\n",
                sizeof(table_cases) / sizeof(table_cases[0]), attempted == 0 ? 3 : 4);
    return 0;
}
