// sluice-grep — bounded streaming literal search.
//
// CLI:
//   sluice-grep [options] <pattern> <file>...
// Options:
//   -n                      prefix matches with 1-based line numbers
//   --buffer-size <bytes>   read buffer (default 1 MiB)
//   --max-line-bytes <n>    retained-line cap (default 1 MiB; longer lines
//                           are reported to stderr and skipped)
//   --workers <count>       Runtime worker count (default 1)
//   --help                  show usage
//
// Backend: ThreadPoolBackend (real file I/O). One ApplicationRuntime for the
// whole batch; files are scanned sequentially in CLI order with ONE reusable
// read buffer. Matches stream to stdout as they are found (they are never
// buffered in memory): memory ~= buffer_size + max_line_bytes + O(1).
//
// Semantics: byte-oriented literal substring match per line ('\n' is the
// only line terminator; NUL bytes and invalid UTF-8 pass through; no
// Unicode/grapheme claims). Empty pattern matches every line. Deterministic
// output: files in CLI order, lines in file order. With more than one input
// file each match is prefixed "path:"; -n adds the line number.
//
// Exit codes (grep tradition, documented in README): 0 = match found,
// 1 = no match, 2 = error. Cancellation (not reachable via the CLI in V1)
// would surface as 2.
#include "cli_parse.hpp"
#include "grep_task.hpp"

#include <sluice/error.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

namespace {

using sluice_grep::GrepInput;
using sluice_grep::cli::CliArgs;
using sluice_grep::cli::parse_args;

struct FdCloser {
    std::vector<int>& fds;
    ~FdCloser() {
        for (int fd : fds)
            if (fd >= 0) ::close(fd);
    }
};

}  // namespace

int main(int argc, char** argv) {
    CliArgs args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0) return rc;
    if (args.help) {
        sluice_grep::cli::usage(argv[0]);
        return 0;
    }

    const bool prefix_name = args.files.size() > 1;

    // Open + validate every input up front (regular files only; positional
    // reads need a seekable source). Failed opens are reported and skipped.
    struct OpenFailure {
        std::size_t cli_index;
        bool not_regular;
        int os_errno;
    };
    std::vector<OpenFailure> failures;
    std::vector<std::size_t> input_cli_index;
    std::vector<GrepInput> inputs;
    std::vector<int> open_fds;
    FdCloser closer{open_fds};
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
        inputs.push_back(GrepInput{path, fd});
        open_fds.push_back(fd);
    }

    // Sink: print each match with the documented prefix rules. stdout is
    // line-buffered enough for a pipe consumer: we fflush per file so a
    // downstream reader sees progress without unbounded buffering.
    auto sink = [&](const std::string& path, std::uint64_t line_no,
                    std::string_view line) {
        if (prefix_name) std::fwrite(path.data(), 1, path.size(), stdout);
        if (prefix_name && args.line_numbers) std::fputc(':', stdout);
        if (args.line_numbers) std::printf("%llu:", static_cast<unsigned long long>(line_no));
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
    };

    auto results =
        sluice_grep::grep_files(args.pattern, std::move(inputs),
                                args.buffer_size, args.max_line_bytes,
                                args.workers, sink);
    std::fflush(stdout);

    // Report in CLI order (failures + engine results), grep-style.
    bool any_error = false;
    bool any_match = false;
    std::size_t fi = 0, gi = 0;
    for (std::size_t i = 0; i < args.files.size(); ++i) {
        if (fi < failures.size() && failures[fi].cli_index == i) {
            const auto& f = failures[fi++];
            if (f.not_regular)
                std::fprintf(stderr, "%s: %s: not a regular file\n", argv[0],
                             args.files[i].c_str());
            else
                std::fprintf(stderr, "%s: %s: %s\n", argv[0],
                             args.files[i].c_str(), std::strerror(f.os_errno));
            any_error = true;
        }
        if (gi < input_cli_index.size() && input_cli_index[gi] == i) {
            const auto& r = results[gi++];
            if (r.match_count > 0) any_match = true;
            if (r.dropped_long_lines)
                std::fprintf(stderr,
                             "%s: %s: line longer than --max-line-bytes "
                             "skipped (not matched)\n",
                             argv[0], r.path.c_str());
            if (r.error.has_value()) {
                std::fprintf(stderr, "%s: %s: %s%s%s\n", argv[0],
                             r.path.c_str(),
                             r.error->code == sluice::IoError::Code::canceled
                                 ? "canceled"
                                 : "read error",
                             r.error->os_errno ? " (" : "",
                             r.error->os_errno
                                 ? std::strerror(r.error->os_errno)
                                 : "");
                any_error = true;
            }
        }
    }

    if (any_error) return 2;
    return any_match ? 0 : 1;
}
