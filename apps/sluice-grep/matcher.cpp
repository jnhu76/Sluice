// sluice-grep — streaming literal line matcher implementation.
#include "matcher.hpp"

#include <algorithm>
#include <cstring>

namespace sluice_grep {

bool line_contains(std::string_view line, std::string_view pattern) {
    if (pattern.empty()) return true;  // empty pattern matches every line
    if (pattern.size() > line.size()) return false;
    return std::search(line.begin(), line.end(), pattern.begin(),
                       pattern.end()) != line.end();
}

LineMatcher::LineMatcher(std::string pattern, std::size_t max_line_bytes)
    : pattern_(std::move(pattern)), max_line_bytes_(max_line_bytes) {
    // Reserve the carry's full budget up front so steady-state feeding never
    // allocates (the buffer only grows up to max_line_bytes_).
    carry_.reserve(max_line_bytes_ + 1);
}

void LineMatcher::feed(const std::uint8_t* data, std::size_t len,
                       std::vector<MatchEvent>& out) {
    std::size_t i = 0;
    while (i < len) {
        if (dropping_) {
            // Inside a too-long line: discard up to and including the next
            // newline, then resume normal assembly.
            const void* nl = std::memchr(data + i, '\n', len - i);
            if (nl == nullptr) return;  // still inside the long line
            std::size_t nl_off = static_cast<const std::uint8_t*>(nl) - data;
            ++line_no_;
            dropping_ = false;
            i = nl_off + 1;
            continue;
        }

        const void* nl = std::memchr(data + i, '\n', len - i);
        if (nl == nullptr) {
            // No newline in the rest of the chunk: append to the carry if it
            // stays within the cap, else enter dropping mode.
            std::size_t rest = len - i;
            if (carry_.size() + rest <= max_line_bytes_) {
                carry_.append(reinterpret_cast<const char*>(data + i), rest);
            } else {
                // Keep the line count honest by remembering the drop; the
                // line number is incremented at its terminating newline.
                dropped_long_ = true;
                dropping_ = true;
                carry_.clear();
            }
            return;
        }

        std::size_t nl_off = static_cast<const std::uint8_t*>(nl) - data;
        std::size_t piece = nl_off - i;
        // Complete line = carry + data[i, nl_off). Only assemble when it fits
        // the cap; an over-cap line is dropped (not matched, not emitted).
        if (!carry_.empty()) {
            if (carry_.size() + piece <= max_line_bytes_) {
                carry_.append(reinterpret_cast<const char*>(data + i), piece);
                ++line_no_;
                if (line_contains(carry_, pattern_))
                    out.push_back(MatchEvent{line_no_, carry_});
                carry_.clear();
            } else {
                ++line_no_;
                dropped_long_ = true;
                dropping_ = false;  // the newline IS this line's terminator
                carry_.clear();
            }
            i = nl_off + 1;
        } else if (piece <= max_line_bytes_) {
            ++line_no_;
            if (piece == 0) {
                // Empty line: matches only the empty pattern.
                if (pattern_.empty()) out.push_back(MatchEvent{line_no_, ""});
            } else {
                std::string_view line(
                    reinterpret_cast<const char*>(data + i), piece);
                if (line_contains(line, pattern_))
                    out.push_back(MatchEvent{line_no_, std::string(line)});
            }
            i = nl_off + 1;
        } else {
            ++line_no_;
            dropped_long_ = true;
            carry_.clear();
            i = nl_off + 1;
        }
    }
}

void LineMatcher::finish(std::vector<MatchEvent>& out) {
    if (dropping_) {
        // The final line was too long AND unterminated: count it and drop.
        ++line_no_;
        dropping_ = false;
        carry_.clear();
        return;
    }
    if (!carry_.empty()) {
        // Final line without a terminating '\n'.
        ++line_no_;
        if (line_contains(carry_, pattern_))
            out.push_back(MatchEvent{line_no_, carry_});
        carry_.clear();
    }
}

}  // namespace sluice_grep
