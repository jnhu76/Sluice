// sluice-grep — streaming literal line matcher (app-local, pure, testable).
//
// Byte-oriented literal substring search organized line-wise: chunks are fed
// incrementally; a line is matched only when complete (its terminating '\n'
// seen, or the final line at EOF without '\n'). Because lines are assembled
// across chunk boundaries before matching, a pattern occurring at any chunk
// boundary within one line is found — the cross-buffer cases fall out of the
// line assembly for free.
//
// Bounded-memory long-line policy: a line longer than max_line_bytes cannot
// be buffered, so it is NOT matched and NOT emitted; scanning resumes at the
// next newline (line numbering stays correct). The caller reports the drop
// via dropped_long_lines(). A pattern longer than max_line_bytes can never
// match a retained line.
//
// A pattern containing '\n' can never match (a single line never contains a
// newline); callers may reject such patterns up front.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sluice_grep {

// One emitted match: the 1-based number of the line and the full line text
// (without the terminating '\n').
struct MatchEvent {
    std::uint64_t line_no;
    std::string line;
};

class LineMatcher {
public:
    // `max_line_bytes` is the inclusive cap on a retained line (the
    // terminating '\n' does not count toward it). A zero cap is clamped to
    // 1 (zero can retain nothing and would otherwise produce a
    // non-advancing over-cap scan).
    LineMatcher(std::string pattern, std::size_t max_line_bytes);

    // Feed the next chunk. Complete matching lines are appended to `out` in
    // order. Non-matching complete lines are dropped. Never allocates beyond
    // the carry buffer (bounded by max_line_bytes).
    void feed(const std::uint8_t* data, std::size_t len,
              std::vector<MatchEvent>& out);

    // Flush at EOF: if the file does not end with '\n' and the carry is
    // non-empty, it is the final line and is matched like any other. If the
    // carry is empty, nothing is emitted.
    void finish(std::vector<MatchEvent>& out);

    // True when at least one line was dropped for exceeding max_line_bytes
    // (reported per file; the caller surfaces it as a diagnostic).
    bool dropped_long_lines() const { return dropped_long_; }

    // Lines ended by '\n' so far (plus the final flushed line if any) — the
    // file's total line count for exit-code-free statistics.
    std::uint64_t complete_lines() const { return line_no_; }

    std::size_t pattern_size() const { return pattern_.size(); }

private:
    // V2 chunk scanner: process the complete-line region [i, end) of one
    // feed buffer (see matcher.cpp). `p` is the feed base pointer; `i` is a
    // line boundary; the carry is empty.
    void scan_complete_region(const char* p, std::size_t i, std::size_t end,
                              std::vector<MatchEvent>& out);

    std::string pattern_;
    std::size_t max_line_bytes_;
    std::string carry_;        // bytes of the current (incomplete) line
    std::size_t anchor_off_ = 0;   // rarest-byte anchor within pattern_
    bool pattern_has_nl_ = false;  // such a pattern can never match a line
    std::uint64_t line_no_ = 0;  // lines completed so far
    bool dropping_ = false;    // inside a too-long line (discard until '\n')
    bool dropped_long_ = false;
};

// Literal substring test (std::search semantics): an empty pattern returns
// true for every line; a non-empty pattern containing '\n' returns false (a
// single line never contains a newline).
bool line_contains(std::string_view line, std::string_view pattern);

}  // namespace sluice_grep
