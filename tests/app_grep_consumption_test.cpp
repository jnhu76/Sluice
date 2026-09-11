#include "grep_task.hpp"

#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice_grep::GrepFileResult;
using sluice_grep::GrepInput;
using sluice_grep::MatchSink;

struct CollectedMatch {
    std::string path;
    std::uint64_t line_no = 0;
    std::string line;
};

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_app_grep_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    std::string out_path = path;
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(content.size())) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            ::close(fd);
            ::unlink(out_path.c_str());
            return {};
        }
        written += n;
    }
    ::close(fd);
    return out_path;
}

bool grep_files_matches_lines_and_counts() {
    const std::string path = make_temp_file("alpha one\nbeta two\nalpha three\n");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;

    std::vector<CollectedMatch> matches;
    MatchSink sink = [&](const std::string& p, std::uint64_t line_no, std::string_view line) {
        matches.push_back(CollectedMatch{p, line_no, std::string(line)});
    };

    std::vector<GrepInput> inputs;
    inputs.push_back(GrepInput{path, std::move(opened).value()});

    const auto results =
        sluice_grep::grep_files("alpha", std::move(inputs), 4096, 1 << 20, 1, sink);
    if (results.size() != 1)
        return false;
    if (results[0].error.has_value())
        return false;
    if (results[0].match_count != 2 || results[0].lines_scanned != 3)
        return false;
    if (results[0].dropped_long_lines)
        return false;
    if (matches.size() != 2)
        return false;
    if (matches[0].line_no != 1 || matches[0].line != "alpha one")
        return false;
    if (matches[1].line_no != 3 || matches[1].line != "alpha three")
        return false;
    return matches[0].path == path && matches[1].path == path;
}

bool grep_files_reports_results_in_input_order() {
    const std::string path_a = make_temp_file("alpha\nplain\n");
    const std::string path_b = make_temp_file("plain\nalpha alpha\n");
    if (path_a.empty() || path_b.empty()) {
        if (!path_a.empty())
            ::unlink(path_a.c_str());
        if (!path_b.empty())
            ::unlink(path_b.c_str());
        return false;
    }
    auto opened_a = File::open(path_a);
    auto opened_b = File::open(path_b);
    ::unlink(path_a.c_str());
    ::unlink(path_b.c_str());
    if (!opened_a.has_value() || !opened_b.has_value())
        return false;

    std::vector<CollectedMatch> matches;
    MatchSink sink = [&](const std::string& p, std::uint64_t line_no, std::string_view line) {
        matches.push_back(CollectedMatch{p, line_no, std::string(line)});
    };

    std::vector<GrepInput> inputs;
    inputs.push_back(GrepInput{path_a, std::move(opened_a).value()});
    inputs.push_back(GrepInput{path_b, std::move(opened_b).value()});

    const auto results =
        sluice_grep::grep_files("alpha", std::move(inputs), 4096, 1 << 20, 1, sink);
    if (results.size() != 2)
        return false;
    if (results[0].path != path_a || results[1].path != path_b)
        return false;
    if (results[0].match_count != 1 || results[1].match_count != 1)
        return false;
    if (matches.size() != 2)
        return false;
    if (matches[0].path != path_a || matches[1].path != path_b)
        return false;
    if (matches[1].line != "alpha alpha")
        return false;
    return matches[0].line_no == 1 && matches[1].line_no == 2;
}

bool grep_files_reports_dropped_long_lines() {
    const std::string long_line(32, 'x');
    const std::string content = long_line + "alpha\nalpha\n";
    const std::string path = make_temp_file(content);
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;

    std::vector<CollectedMatch> matches;
    MatchSink sink = [&](const std::string& p, std::uint64_t line_no, std::string_view line) {
        matches.push_back(CollectedMatch{p, line_no, std::string(line)});
    };

    std::vector<GrepInput> inputs;
    inputs.push_back(GrepInput{path, std::move(opened).value()});

    const auto results =
        sluice_grep::grep_files("alpha", std::move(inputs), 4096, 16, 1, sink);
    if (results.size() != 1)
        return false;
    if (results[0].error.has_value())
        return false;
    if (!results[0].dropped_long_lines)
        return false;
    if (results[0].match_count != 1)
        return false;
    return matches.size() == 1 && matches[0].line == "alpha";
}

bool grep_files_reports_error_for_closed_file() {
    const std::string path = make_temp_file("alpha\n");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    if (!file.close().has_value())
        return false;

    std::vector<CollectedMatch> matches;
    MatchSink sink = [&](const std::string& p, std::uint64_t line_no, std::string_view line) {
        matches.push_back(CollectedMatch{p, line_no, std::string(line)});
    };

    std::vector<GrepInput> inputs;
    inputs.push_back(GrepInput{path, std::move(file)});

    const auto results =
        sluice_grep::grep_files("alpha", std::move(inputs), 4096, 1 << 20, 1, sink);
    if (results.size() != 1)
        return false;
    if (!results[0].error.has_value())
        return false;
    return matches.empty();
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"grep_files_matches_lines_and_counts", grep_files_matches_lines_and_counts},
        {"grep_files_reports_results_in_input_order",
         grep_files_reports_results_in_input_order},
        {"grep_files_reports_dropped_long_lines", grep_files_reports_dropped_long_lines},
        {"grep_files_reports_error_for_closed_file",
         grep_files_reports_error_for_closed_file},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu app grep consumption tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
