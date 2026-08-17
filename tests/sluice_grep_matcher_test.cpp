// sluice-grep matcher tests: the streaming LineMatcher against every
// boundary condition in the brief — cross-buffer patterns, cross-buffer
// lines, final line without newline, empty file, empty pattern, long-line
// dropping with correct numbering.
#include "harness.hpp"

#include "matcher.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace sluice_grep;

namespace {

// Feed `data` through the matcher in `chunk`-sized pieces; return matches.
// Also exercises finish().
std::vector<MatchEvent> run_match(const std::string& pattern,
                                  const std::string& data,
                                  std::size_t chunk,
                                  std::size_t max_line_bytes = 1 << 20) {
    LineMatcher m(pattern, max_line_bytes);
    std::vector<MatchEvent> out;
    for (std::size_t i = 0; i < data.size(); i += chunk) {
        std::size_t n = std::min(chunk, data.size() - i);
        m.feed(reinterpret_cast<const std::uint8_t*>(data.data()) + i, n, out);
    }
    m.finish(out);
    return out;
}

// Require that every chunk size produces exactly the expected (line,line_no)
// pairs — chunk-invariance IS the cross-buffer correctness proof.
void expect_matches(const std::string& pattern, const std::string& data,
                    const std::vector<std::pair<std::uint64_t, std::string>>& exp) {
    const std::size_t chunks[] = {1, 2, 3, 5, 7, 8, 15, 16, 17,
                                  63, 64, 65, 1000, 1 << 20};
    for (std::size_t c : chunks) {
        auto got = run_match(pattern, data, c);
        if (got.size() != exp.size()) {
            SLUICE_CHECK_MSG(false, "chunk-size match-count mismatch");
            return;
        }
        for (std::size_t i = 0; i < exp.size(); ++i) {
            if (got[i].line_no != exp[i].first || got[i].line != exp[i].second) {
                SLUICE_CHECK_MSG(false, "chunk-size match mismatch");
                return;
            }
        }
    }
    SLUICE_CHECK(true);
}

}  // namespace

SLUICE_TEST_CASE(matcher_single_and_multiple_matches) {
    expect_matches("hello", "hello world\nsay hello twice\nnope\n",
                   {{1, "hello world"}, {2, "say hello twice"}});
}

SLUICE_TEST_CASE(matcher_no_match) {
    expect_matches("xyz", "aaa\nbbb\nccc\n", {});
}

SLUICE_TEST_CASE(matcher_cross_buffer_line_assembly) {
    // A single line longer than any test chunk: the match positions straddle
    // every chunk boundary as `chunk` varies.
    std::string line = std::string(200, 'x') + "NEEDLE" + std::string(200, 'x');
    expect_matches("NEEDLE", line + "\nsecond\n", {{1, line}, });
}

SLUICE_TEST_CASE(matcher_pattern_split_across_buffers) {
    // The needle itself is split by the chunking (covered by the chunk sweep
    // in expect_matches; this pins the intent with a tight needle).
    expect_matches("abc", "xxab", {});  // sanity: incomplete tail is not a match
    expect_matches("abc", "zzabczz\n", {{1, "zzabczz"}});
}

SLUICE_TEST_CASE(matcher_final_line_without_newline) {
    expect_matches("end", "first\nmiddle\nthe end", {{3, "the end"}});
    // File that ends exactly at a newline: no phantom final line.
    expect_matches("first", "first\n", {{1, "first"}});
}

SLUICE_TEST_CASE(matcher_empty_file) {
    expect_matches("anything", "", {});
}

SLUICE_TEST_CASE(matcher_empty_pattern_matches_every_line) {
    expect_matches("", "one\ntwo\n\nfour", {{1, "one"},
                                            {2, "two"},
                                            {3, ""},
                                            {4, "four"}});
}

SLUICE_TEST_CASE(matcher_line_numbers_count_all_lines) {
    LineMatcher m("only", 1 << 20);
    std::vector<MatchEvent> out;
    std::string data = "skip\nskip\nonly me\nskip\nonly again\n";
    m.feed(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), out);
    m.finish(out);
    SLUICE_CHECK(out.size() == 2);
    SLUICE_CHECK(out[0].line_no == 3 && out[0].line == "only me");
    SLUICE_CHECK(out[1].line_no == 5 && out[1].line == "only again");
    SLUICE_CHECK(m.complete_lines() == 5);
}

SLUICE_TEST_CASE(matcher_long_line_dropped_numbering_stays_correct) {
    // Cap 16 bytes; line 2 is 40 bytes (dropped); lines 1/3 still match and
    // keep their true numbers.
    std::string data = "hit one\n" + std::string(40, 'z') + "\nhit three\n";
    LineMatcher m("hit", 16);
    std::vector<MatchEvent> out;
    m.feed(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), out);
    m.finish(out);
    SLUICE_CHECK(out.size() == 2);
    SLUICE_CHECK(out[0].line_no == 1 && out[0].line == "hit one");
    SLUICE_CHECK(out[1].line_no == 3 && out[1].line == "hit three");
    SLUICE_CHECK(m.dropped_long_lines());
    SLUICE_CHECK(m.complete_lines() == 3);
}

SLUICE_TEST_CASE(matcher_long_line_spanning_many_chunks_dropped) {
    // A 300-byte line fed 7 bytes at a time under a 16-byte cap: the drop
    // path must survive chunked feeding and still number later lines.
    LineMatcher m("needle", 16);
    std::vector<MatchEvent> out;
    std::string data = std::string(300, 'q') + "\nneedle here\n";
    for (std::size_t i = 0; i < data.size(); i += 7) {
        std::size_t n = std::min<std::size_t>(7, data.size() - i);
        m.feed(reinterpret_cast<const std::uint8_t*>(data.data()) + i, n, out);
    }
    m.finish(out);
    SLUICE_CHECK(out.size() == 1);
    SLUICE_CHECK(out[0].line_no == 2 && out[0].line == "needle here");
    SLUICE_CHECK(m.dropped_long_lines());
}

SLUICE_TEST_CASE(matcher_unterminated_long_line_dropped_at_finish) {
    // A final unterminated line beyond the cap: dropped, counted, no output.
    // ("ok" contains no 'x', so line 1 must not match either.)
    LineMatcher m("x", 8);
    std::vector<MatchEvent> out;
    std::string data = "ok\n" + std::string(50, 'x');  // no trailing newline
    m.feed(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), out);
    m.finish(out);
    SLUICE_CHECK(out.size() == 0);
    SLUICE_CHECK(m.dropped_long_lines());
    SLUICE_CHECK(m.complete_lines() == 2);
}

SLUICE_TEST_CASE(matcher_empty_lines_and_needle_larger_than_line) {
    expect_matches("longer-than-line", "a\n\nb\n", {});
}

SLUICE_MAIN()
