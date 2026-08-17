// sluice-hash — bounded streaming file hashing.
//
// CLI:
//   sluice-hash [options] <file>...
// Options:
//   --buffer-size <bytes>   read buffer (default 1 MiB; 4 KiB..64 MiB)
//   --workers <count>       Runtime worker count (default 1)
//   --help                  show usage
//
// Backend: ThreadPoolBackend (real file I/O). One ApplicationRuntime for the
// whole batch; files are hashed sequentially in CLI order, one reusable
// buffer for all of them (memory ~ 1 x buffer_size + O(1) hasher state,
// independent of file count/size). Algorithm: SHA-256 (app-local FIPS 180-4
// implementation, NIST-vector-anchored — see sha256.hpp).
//
// Input domain: every input must be a REGULAR file (positional reads need a
// seekable source). An unreadable/non-regular input is reported to stderr and
// skipped; hashing continues with the remaining files.
//
// Output: "<digest>  <filename>" per successful file on stdout (the
// sha256sum-compatible shape). Diagnostics on stderr only.
//
// Exit codes: 0 = all hashed, 1 = usage error, 2 = I/O failure (at least one
// file), 3 = canceled.
#include "cli_parse.hpp"
#include "hash_task.hpp"

#include <sluice/error.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

namespace {

using sluice_hash::FileHash;
using sluice_hash::HashInput;
using sluice_hash::cli::CliArgs;
using sluice_hash::cli::parse_args;

// Batch closer for the fds handed to the hashing engine: hash_files uses the
// fds but never closes them; this closes everything still open at scope exit
// (including early error returns). A vector of non-movable RAII guard objects
// cannot reallocate, so the fd list is plain ints instead.
struct FdCloser {
    std::vector<int>& fds;
    ~FdCloser() {
        for (int fd : fds)
            if (fd >= 0) ::close(fd);
    }
};

const char* errno_msg(int e) { return std::strerror(e); }

}  // namespace

int main(int argc, char** argv) {
    CliArgs args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0) return rc;
    if (args.help) {
        sluice_hash::cli::usage(argv[0]);
        return 0;
    }

    // Open + validate every input up front (O_RDONLY, fstat, regular file).
    // Failed opens are reported here and never enter the hashing engine; the
    // remaining files still hash (sha256sum-style error isolation).
    struct OpenFailure {
        std::size_t cli_index;
        bool not_regular;
        int os_errno;
    };
    std::vector<OpenFailure> failures;
    std::vector<std::size_t> input_cli_index;  // results[i] belongs to this file
    std::vector<HashInput> inputs;
    std::vector<int> open_fds;  // successfully opened, engine-visible fds
    FdCloser closer{open_fds};
    input_cli_index.reserve(args.files.size());
    inputs.reserve(args.files.size());
    open_fds.reserve(args.files.size());

    for (std::size_t i = 0; i < args.files.size(); ++i) {
        const std::string& path = args.files[i];
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            failures.push_back({i, false, errno});
            continue;
        }
        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            failures.push_back({i, false, errno});
            ::close(fd);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            failures.push_back({i, true, 0});
            ::close(fd);
            continue;
        }
        input_cli_index.push_back(i);
        inputs.push_back(HashInput{path, fd});
        open_fds.push_back(fd);
    }

    auto results = sluice_hash::hash_files(std::move(inputs), args.buffer_size,
                                           args.workers);

    // Report in CLI order: merge engine results and open failures (both were
    // recorded in CLI order, so two cursors suffice).
    bool any_error = false;
    bool any_canceled = false;
    std::size_t fi = 0;   // next open failure
    std::size_t gi = 0;   // next engine result (input_cli_index[gi] names it)
    for (std::size_t i = 0; i < args.files.size(); ++i) {
        if (fi < failures.size() && failures[fi].cli_index == i) {
            const auto& f = failures[fi++];
            if (f.not_regular) {
                std::fprintf(stderr, "%s: %s: not a regular file\n", argv[0],
                             args.files[i].c_str());
            } else {
                std::fprintf(stderr, "%s: %s: %s\n", argv[0],
                             args.files[i].c_str(), errno_msg(f.os_errno));
            }
            any_error = true;
        }
        if (gi < input_cli_index.size() && input_cli_index[gi] == i) {
            const FileHash& r = results[gi++];
            if (r.error.has_value()) {
                bool canceled =
                    r.error->code == sluice::IoError::Code::canceled;
                std::fprintf(stderr, "%s: %s: %s%s%s\n", argv[0],
                             r.path.c_str(),
                             canceled ? "canceled" : "read error",
                             r.error->os_errno ? " (" : "",
                             r.error->os_errno ? errno_msg(r.error->os_errno)
                                               : "");
                any_error = true;
                if (canceled) any_canceled = true;
                continue;
            }
            std::printf("%s  %s\n", r.hex.c_str(), r.path.c_str());
        }
    }

    if (any_canceled) return 3;
    if (any_error) return 2;
    return 0;
}
