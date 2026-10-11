#include "grep_task.hpp"

#include <sluice/file_resource.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <stdexcept>

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

    const auto results = sluice_grep::grep_files("alpha", std::move(inputs), 4096, 16, 1, sink);
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

std::vector<GrepFileResult> invoke(unsigned kind, const std::string& pattern,
                                   std::vector<GrepInput> inputs, std::size_t buffer,
                                   std::size_t cap, MatchSink sink) {
    if (kind == 3)
        return sluice_grep::grep_files_with_backend(
            pattern, std::move(inputs), buffer, cap, 1, std::move(sink),
            std::make_unique<sluice::async::ThreadPoolBackend>());
    constexpr unsigned workers[] = {1, 2, 64};
    return sluice_grep::grep_files(pattern, std::move(inputs), buffer, cap, workers[kind],
                                   std::move(sink));
}

bool chunk_invariance_and_byte_line_oracle() {
    const std::string content = "\n" + std::string(4092, 'x') + "alpha\r\n" +
                                std::string("\0alpha\xff\n", 8) + std::string(9000, 'x') +
                                "alpha\nalpha tail";
    for (unsigned kind = 0; kind != 4; ++kind) {
        for (std::size_t buffer : {4096, 4097, 8192}) {
            const auto path = make_temp_file(content);
            if (path.empty())
                return false;
            auto opened = File::open(path);
            ::unlink(path.c_str());
            if (!opened.has_value())
                return false;
            std::vector<GrepInput> inputs;
            inputs.push_back({path, std::move(opened).value()});
            std::vector<CollectedMatch> matches;
            const auto results =
                invoke(kind, "alpha", std::move(inputs), buffer, 5000,
                       [&](const std::string& p, std::uint64_t n, std::string_view line) {
                           matches.push_back({p, n, std::string(line)});
                       });
            if (results.size() != 1 || results[0].error || results[0].match_count != 3 ||
                results[0].lines_scanned != 5 || !results[0].dropped_long_lines ||
                matches.size() != 3 || matches[0].line_no != 2 ||
                matches[0].line != std::string(4092, 'x') + "alpha\r" || matches[1].line_no != 3 ||
                matches[1].line != std::string("\0alpha\xff", 7) || matches[2].line_no != 5 ||
                matches[2].line != "alpha tail")
                return false;
        }
    }
    return true;
}

bool read_failure_after_output_continues_to_next_file() {
    for (unsigned kind = 0; kind != 4; ++kind) {
        const auto path = make_temp_file("alpha\n" + std::string(8192, 'x') + "\nalpha tail");
        if (path.empty())
            return false;
        auto first = File::open(path);
        auto second = File::open(path);
        ::unlink(path.c_str());
        if (!first.has_value() || !second.has_value())
            return false;
        std::vector<GrepInput> inputs;
        inputs.push_back({"first", std::move(first).value()});
        inputs.push_back({"second", std::move(second).value()});
        File* first_file = &inputs[0].file;
        std::vector<CollectedMatch> matches;
        bool closed = false;
        const auto results =
            invoke(kind, "alpha", std::move(inputs), 4096, 16384,
                   [&](const std::string& p, std::uint64_t n, std::string_view line) {
                       matches.push_back({p, n, std::string(line)});
                       if (p == "first")
                           closed = first_file->close().has_value();
                   });
        if (!closed || results.size() != 2 || !results[0].error ||
            results[0].error->code != sluice::IoError::Code::invalid_state ||
            results[0].match_count != 1 || results[0].lines_scanned != 1 || results[1].error ||
            results[1].match_count != 2 || matches.size() != 3 || matches[0].path != "first" ||
            matches[1].path != "second" || matches[2].path != "second" ||
            matches[2].line != "alpha tail")
            return false;
    }
    return true;
}

bool invalid_config_has_no_sink_effect() {
    struct Config {
        std::size_t buffer;
        std::size_t cap;
        unsigned workers;
    };
    const Config configs[] = {{4095, 1, 1},        {67108865, 1, 1}, {4096, 0, 1},
                              {4096, 67108865, 1}, {4096, 1, 0},     {4096, 1, 65}};
    for (const auto config : configs) {
        const auto path = make_temp_file("alpha\n");
        if (path.empty())
            return false;
        auto opened = File::open(path);
        ::unlink(path.c_str());
        if (!opened.has_value())
            return false;
        std::vector<GrepInput> inputs;
        inputs.push_back({path, std::move(opened).value()});
        bool called = false;
        auto results = sluice_grep::grep_files(
            "alpha", std::move(inputs), config.buffer, config.cap, config.workers,
            [&](const std::string&, std::uint64_t, std::string_view) { called = true; });
        if (called || results.size() != 1 || !results[0].error ||
            results[0].error->code != sluice::IoError::Code::invalid_state)
            return false;
    }
    return true;
}

class CanceledReadBackend final : public sluice::async::ThreadPoolBackend {
    sluice::Result<sluice::async::detail::RequestKey>
    submit_read(sluice::async::ReadOp, sluice::async::Completion<std::size_t>*) override {
        return sluice::make_unexpected<sluice::async::detail::RequestKey>(
            sluice::IoError{sluice::IoError::Code::canceled});
    }
};

bool injected_canceled_read_has_no_match() {
    const auto path = make_temp_file("alpha\n");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;
    std::vector<GrepInput> inputs;
    inputs.push_back({path, std::move(opened).value()});
    bool called = false;
    const auto results = sluice_grep::grep_files_with_backend(
        "alpha", std::move(inputs), 4096, 16384, 1,
        [&](const std::string&, std::uint64_t, std::string_view) { called = true; },
        std::make_unique<CanceledReadBackend>());
    return !called && results.size() == 1 && results[0].error &&
           results[0].error->code == sluice::IoError::Code::canceled &&
           results[0].match_count == 0;
}

bool sink_exception_settles_before_return() {
    for (unsigned kind = 0; kind != 4; ++kind) {
        const auto path = make_temp_file("alpha\n" + std::string(8192, 'x'));
        if (path.empty())
            return false;
        auto opened = File::open(path);
        ::unlink(path.c_str());
        if (!opened.has_value())
            return false;
        std::vector<GrepInput> inputs;
        inputs.push_back({path, std::move(opened).value()});
        bool called = false;
        auto results = invoke(kind, "alpha", std::move(inputs), 4096, 16384,
                              [&](const std::string&, std::uint64_t, std::string_view) {
                                  called = true;
                                  throw std::runtime_error("sink failure");
                              });
        if (!called || results.size() != 1 || !results[0].error)
            return false;
    }
    return true;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"injected_canceled_read_has_no_match", injected_canceled_read_has_no_match},
        {"chunk_invariance_and_byte_line_oracle", chunk_invariance_and_byte_line_oracle},
        {"read_failure_after_output_continues_to_next_file",
         read_failure_after_output_continues_to_next_file},
        {"invalid_config_has_no_sink_effect", invalid_config_has_no_sink_effect},
        {"sink_exception_settles_before_return", sink_exception_settles_before_return},
        {"grep_files_matches_lines_and_counts", grep_files_matches_lines_and_counts},
        {"grep_files_reports_results_in_input_order", grep_files_reports_results_in_input_order},
        {"grep_files_reports_dropped_long_lines", grep_files_reports_dropped_long_lines},
        {"grep_files_reports_error_for_closed_file", grep_files_reports_error_for_closed_file},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu app grep consumption tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
