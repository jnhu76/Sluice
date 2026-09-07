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

struct FdCloser {
    std::vector<int>& fds;
    ~FdCloser() {
        for (int fd : fds)
            if (fd >= 0)
                ::close(fd);
    }
};

const char* errno_msg(int e) {
    return std::strerror(e);
}

} // namespace

int main(int argc, char** argv) {
    CliArgs args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0)
        return rc;
    if (args.help) {
        sluice_hash::cli::usage(argv[0]);
        return 0;
    }

    struct OpenFailure {
        std::size_t cli_index;
        bool not_regular;
        int os_errno;
    };
    std::vector<OpenFailure> failures;
    std::vector<std::size_t> input_cli_index;
    std::vector<HashInput> inputs;
    std::vector<int> open_fds;
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

    auto results = sluice_hash::hash_files(std::move(inputs), args.buffer_size, args.workers);

    bool any_error = false;
    bool any_canceled = false;
    std::size_t fi = 0;
    std::size_t gi = 0;
    for (std::size_t i = 0; i < args.files.size(); ++i) {
        if (fi < failures.size() && failures[fi].cli_index == i) {
            const auto& f = failures[fi++];
            if (f.not_regular) {
                std::fprintf(stderr, "%s: %s: not a regular file\n", argv[0],
                             args.files[i].c_str());
            } else {
                std::fprintf(stderr, "%s: %s: %s\n", argv[0], args.files[i].c_str(),
                             errno_msg(f.os_errno));
            }
            any_error = true;
        }
        if (gi < input_cli_index.size() && input_cli_index[gi] == i) {
            const FileHash& r = results[gi++];
            if (r.error.has_value()) {
                bool canceled = r.error->code == sluice::IoError::Code::canceled;
                std::fprintf(stderr, "%s: %s: %s%s%s\n", argv[0], r.path.c_str(),
                             canceled ? "canceled" : "read error", r.error->os_errno ? " (" : "",
                             r.error->os_errno ? errno_msg(r.error->os_errno) : "");
                any_error = true;
                if (canceled)
                    any_canceled = true;
                continue;
            }
            std::printf("%s  %s\n", r.hex.c_str(), r.path.c_str());
        }
    }

    if (any_canceled)
        return 3;
    if (any_error)
        return 2;
    return 0;
}
