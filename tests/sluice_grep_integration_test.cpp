// sluice-grep integration tests: real files + real ThreadPoolBackend through
// the SAME engine the CLI uses (grep_task.cpp + matcher.cpp compiled in).
// Covers: multi-file deterministic ordering (CLI order, line order), sink
// streaming, cross-chunk boundaries with a real pread-driven buffer, final
// line without newline, empty file, no-match, empty pattern, bad-fd error
// isolation.
#include "harness.hpp"

#include "grep_task.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace sluice_grep;

namespace {

struct NamedTemp {
    int fd;
    std::string name;
    NamedTemp(const char* n) : name(n) {
        char p[] = "/tmp/sluice_grep_itest_XXXXXX";
        fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::unlink(p);
    }
    void write(const std::string& s) {
        SLUICE_CHECK(::pwrite(fd, s.data(), s.size(), 0) ==
                     static_cast<ssize_t>(s.size()));
    }
    ~NamedTemp() { if (fd >= 0) ::close(fd); }
    NamedTemp(const NamedTemp&) = delete;
    NamedTemp& operator=(const NamedTemp&) = delete;
};

struct Capture {
    std::vector<std::string> lines;  // "path:line_no:line"
    void add(const std::string& path, std::uint64_t line_no,
             std::string_view line) {
        lines.push_back(path + ":" + std::to_string(line_no) + ":" +
                        std::string(line));
    }
    // The sink handed to the engine MUST write through a reference (a plain
    // functor would be COPIED into the std::function and accumulate matches
    // in the copy).
    sluice_grep::MatchSink sink() {
        return [this](const std::string& p, std::uint64_t n, std::string_view l) {
            add(p, n, l);
        };
    }
};

}  // namespace

SLUICE_TEST_CASE(grep_integration_single_file_matches) {
    NamedTemp f("f");
    f.write("alpha needle one\nplain\nbeta needle two\nneedle alone\n");
    Capture cap;
    auto rs = grep_files("needle", {GrepInput{f.name, f.fd}}, 4096,
                         kDefaultMaxLineBytes, 1, cap.sink());
    SLUICE_CHECK(rs.size() == 1 && !rs[0].error.has_value());
    SLUICE_CHECK(rs[0].match_count == 3);
    SLUICE_CHECK(rs[0].lines_scanned == 4);
    SLUICE_CHECK(cap.lines.size() == 3);
    SLUICE_CHECK(cap.lines[0] == f.name + ":1:alpha needle one");
    SLUICE_CHECK(cap.lines[1] == f.name + ":3:beta needle two");
    SLUICE_CHECK(cap.lines[2] == f.name + ":4:needle alone");
}

SLUICE_TEST_CASE(grep_integration_multi_file_deterministic_order) {
    NamedTemp a("a"), b("b"), c("c");
    a.write("match in a\nno\n");
    b.write("no\nmatch in b\nmatch again in b\n");
    c.write("nothing here\n");
    Capture cap;
    auto rs = grep_files("match",
                         {GrepInput{a.name, a.fd}, GrepInput{b.name, b.fd},
                          GrepInput{c.name, c.fd}},
                         4096, kDefaultMaxLineBytes, 1, cap.sink());
    SLUICE_CHECK(rs.size() == 3);
    for (auto& r : rs) SLUICE_CHECK(!r.error.has_value());
    SLUICE_CHECK(cap.lines.size() == 3);
    // CLI file order, then line order — never interleaved unpredictably.
    SLUICE_CHECK(cap.lines[0] == a.name + ":1:match in a");
    SLUICE_CHECK(cap.lines[1] == b.name + ":2:match in b");
    SLUICE_CHECK(cap.lines[2] == b.name + ":3:match again in b");
    SLUICE_CHECK(rs[2].match_count == 0);
}

SLUICE_TEST_CASE(grep_integration_cross_chunk_streaming) {
    // 100 KiB of 70-byte lines with a needle near each line end; scanned
    // through a 4 KiB buffer (~25 reads): lines AND needles straddle chunk
    // boundaries.
    NamedTemp f("big");
    std::string data;
    int expected = 0;
    for (int i = 0; i < 1500; ++i) {
        std::string line = "line-" + std::to_string(i) + "-" +
                           std::string(60, '.') + "\n";
        if (i % 7 == 0) {
            line = "line-" + std::to_string(i) + "-" +
                   std::string(55, '.') + "NEEDLE\n";
            ++expected;
        }
        data += line;
    }
    f.write(data);

    Capture cap;
    auto rs = grep_files("NEEDLE", {GrepInput{f.name, f.fd}}, 4096,
                         kDefaultMaxLineBytes, 2, cap.sink());
    SLUICE_CHECK(!rs[0].error.has_value());
    SLUICE_CHECK(rs[0].match_count == static_cast<std::uint64_t>(expected));
    SLUICE_CHECK(rs[0].lines_scanned == 1500);
    // Every emitted line is the exact source line (endsWith NEEDLE).
    for (auto& l : cap.lines) {
        SLUICE_CHECK(l.size() > 6 &&
                     l.compare(l.size() - 6, 6, "NEEDLE") == 0);
    }
}

SLUICE_TEST_CASE(grep_integration_final_line_without_newline_and_empty) {
    NamedTemp f("f"), e("e");
    f.write("first\nlast no newline");
    Capture cap;
    auto rs = grep_files("last", {GrepInput{f.name, f.fd}}, 4096,
                         kDefaultMaxLineBytes, 1, cap.sink());
    SLUICE_CHECK(rs[0].match_count == 1);
    SLUICE_CHECK(cap.lines.size() == 1);
    SLUICE_CHECK(cap.lines[0] == f.name + ":2:last no newline");
    SLUICE_CHECK(rs[0].lines_scanned == 2);

    Capture cap2;
    auto rs2 = grep_files("x", {GrepInput{e.name, e.fd}}, 4096,
                          kDefaultMaxLineBytes, 1, cap2.sink());
    SLUICE_CHECK(!rs2[0].error.has_value());
    SLUICE_CHECK(rs2[0].match_count == 0);
    SLUICE_CHECK(rs2[0].lines_scanned == 0);
}

SLUICE_TEST_CASE(grep_integration_empty_pattern_matches_all_lines) {
    NamedTemp f("f");
    f.write("one\ntwo\n\nfour");
    Capture cap;
    auto rs = grep_files("", {GrepInput{f.name, f.fd}}, 4096,
                         kDefaultMaxLineBytes, 1, cap.sink());
    SLUICE_CHECK(rs[0].match_count == 4);  // incl. the empty line + final line
    SLUICE_CHECK(rs[0].lines_scanned == 4);
}

SLUICE_TEST_CASE(grep_integration_bad_fd_isolates_per_file) {
    NamedTemp good("good");
    good.write("needle works\n");
    Capture cap;
    auto rs = grep_files("needle",
                         {GrepInput{"bad", -1}, GrepInput{good.name, good.fd}},
                         4096, kDefaultMaxLineBytes, 1, cap.sink());
    SLUICE_CHECK(rs.size() == 2);
    SLUICE_CHECK(rs[0].error.has_value());
    SLUICE_CHECK(rs[0].match_count == 0);
    SLUICE_CHECK(!rs[1].error.has_value());
    SLUICE_CHECK(rs[1].match_count == 1);
    SLUICE_CHECK(cap.lines.size() == 1);
    SLUICE_CHECK(cap.lines[0] == good.name + ":1:needle works");
}

SLUICE_MAIN()
