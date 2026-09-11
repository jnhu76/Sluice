#include "cli_parse.hpp"
#include "grep_task.hpp"

#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

namespace {

using sluice_grep::GrepInput;
using sluice_grep::cli::CliArgs;
using sluice_grep::cli::parse_args;

} // namespace

int main(int argc, char** argv) {
    CliArgs args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0)
        return rc;
    if (args.help) {
        sluice_grep::cli::usage(argv[0]);
        return 0;
    }

    const bool prefix_name = args.files.size() > 1;

    struct OpenFailure {
        std::size_t cli_index;
        bool not_regular;
        int os_errno;
    };
    std::vector<OpenFailure> failures;
    std::vector<std::size_t> input_cli_index;
    std::vector<GrepInput> inputs;
    inputs.reserve(args.files.size());

    for (std::size_t i = 0; i < args.files.size(); ++i) {
        const std::string& path = args.files[i];
        auto opened = sluice::File::open(path);
        if (!opened.has_value()) {
            failures.push_back({i, false, opened.error().os_errno});
            continue;
        }
        struct stat st{};
        if (::fstat(opened.value().native_handle(), &st) != 0) {
            failures.push_back({i, false, errno});
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            failures.push_back({i, true, 0});
            continue;
        }
        input_cli_index.push_back(i);
        inputs.push_back(GrepInput{path, std::move(opened).value()});
    }

    const bool line_flush = ::isatty(STDOUT_FILENO);
    auto sink = [&](const std::string& path, std::uint64_t line_no, std::string_view line) {
        if (prefix_name)
            std::fwrite(path.data(), 1, path.size(), stdout);
        if (prefix_name && args.line_numbers)
            std::fputc(':', stdout);
        if (args.line_numbers)
            std::printf("%llu:", static_cast<unsigned long long>(line_no));
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        if (line_flush)
            std::fflush(stdout);
    };

    auto results = sluice_grep::grep_files(args.pattern, std::move(inputs), args.buffer_size,
                                           args.max_line_bytes, args.workers, sink);
    std::fflush(stdout);

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
                std::fprintf(stderr, "%s: %s: %s\n", argv[0], args.files[i].c_str(),
                             std::strerror(f.os_errno));
            any_error = true;
        }
        if (gi < input_cli_index.size() && input_cli_index[gi] == i) {
            const auto& r = results[gi++];
            if (r.match_count > 0)
                any_match = true;
            if (r.dropped_long_lines)
                std::fprintf(stderr,
                             "%s: %s: line longer than --max-line-bytes "
                             "skipped (not matched)\n",
                             argv[0], r.path.c_str());
            if (r.error.has_value()) {
                std::fprintf(stderr, "%s: %s: %s%s%s\n", argv[0], r.path.c_str(),
                             r.error->code == sluice::IoError::Code::canceled ? "canceled"
                                                                              : "read error",
                             r.error->os_errno ? " (" : "",
                             r.error->os_errno ? std::strerror(r.error->os_errno) : "");
                any_error = true;
            }
        }
    }

    if (any_error)
        return 2;
    return any_match ? 0 : 1;
}
