#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "matcher.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace sluice_grep {

namespace {

inline int byte_freq_score(unsigned char c) {
    switch (c) {
    case ' ':
        return 10;
    case 'e':
    case 'E':
        return 9;
    case 't':
    case 'T':
        return 8;
    case 'a':
    case 'A':
    case 'o':
    case 'O':
        return 7;
    case 'i':
    case 'I':
    case 'n':
    case 'N':
        return 6;
    case 's':
    case 'S':
    case 'h':
    case 'H':
        return 5;
    case 'r':
    case 'R':
        return 4;
    case 'd':
    case 'l':
    case 'u':
    case 'U':
        return 3;
    case 'c':
    case 'm':
    case 'w':
    case 'f':
    case 'g':
    case 'y':
    case 'p':
    case 'b':
    case 'C':
    case 'M':
    case 'W':
    case 'F':
    case 'G':
    case 'Y':
    case 'P':
    case 'B':
        return 2;
    case '\n':
        return 5;
    default:
        if (c >= '0' && c <= '9')
            return 2;
        if (c >= 'a' && c <= 'z')
            return 1;
        if (c >= 'A' && c <= 'Z')
            return 1;
        if (c >= 0x80)
            return 0;
        if (c < 0x20)
            return 1;
        return 1;
    }
}

inline std::uint64_t count_newlines(const char* p, std::size_t n) {
    constexpr std::uint64_t kOnes = 0x0101010101010101ULL;
    constexpr std::uint64_t kHighs = 0x8080808080808080ULL;
    constexpr std::uint64_t kXor = 0x0A0A0A0A0A0A0A0AULL;
    auto word_hits = [](std::uint64_t w) {
        w ^= kXor;
        return ~(w | ((w | kHighs) - kOnes)) & kHighs;
    };
    std::uint64_t count = 0;
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        std::uint64_t w0, w1, w2, w3;
        std::memcpy(&w0, p + i, 8);
        std::memcpy(&w1, p + i + 8, 8);
        std::memcpy(&w2, p + i + 16, 8);
        std::memcpy(&w3, p + i + 24, 8);
        count += std::popcount(word_hits(w0)) + std::popcount(word_hits(w1)) +
                 std::popcount(word_hits(w2)) + std::popcount(word_hits(w3));
    }
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        count += std::popcount(word_hits(w));
    }
    for (; i < n; ++i)
        count += (p[i] == '\n');
    return count;
}

std::size_t pick_anchor(std::string_view pat) {
    std::size_t anchor = 0;
    int best = byte_freq_score(static_cast<unsigned char>(pat[0]));
    for (std::size_t i = 1; i < pat.size(); ++i) {
        int sc = byte_freq_score(static_cast<unsigned char>(pat[i]));
        if (sc < best) {
            best = sc;
            anchor = i;
        }
    }
    return anchor;
}

std::size_t fast_find(std::string_view hay, std::size_t pos, std::string_view pat,
                      std::size_t anchor_off) {
    const std::size_t n = hay.size();
    if (pos + pat.size() > n)
        return std::string_view::npos;
    const unsigned char anchor = static_cast<unsigned char>(pat[anchor_off]);
    const char* base = hay.data();
    std::size_t p = pos;
    while (p + pat.size() <= n) {
        const void* hit = std::memchr(base + p + anchor_off, anchor, n - pat.size() - p + 1);
        if (hit == nullptr)
            return std::string_view::npos;
        std::size_t a = static_cast<std::size_t>(static_cast<const char*>(hit) - base) - anchor_off;
        if (std::memcmp(base + a, pat.data(), pat.size()) == 0)
            return a;
        p = a + 1;
    }
    return std::string_view::npos;
}

} // namespace

bool line_contains(std::string_view line, std::string_view pattern) {
    if (pattern.empty())
        return true;
    if (pattern.size() > line.size())
        return false;
    if (pattern.find('\n') != std::string_view::npos)
        return false;
    return fast_find(line, 0, pattern, pick_anchor(pattern)) != std::string_view::npos;
}

LineMatcher::LineMatcher(std::string pattern, std::size_t max_line_bytes)
    : pattern_(std::move(pattern)),

      max_line_bytes_(max_line_bytes ? max_line_bytes : 1), anchor_off_(0),
      pattern_has_nl_(pattern_.find('\n') != std::string::npos) {
    carry_.reserve(max_line_bytes_ + 1);
    if (!pattern_.empty())
        anchor_off_ = pick_anchor(pattern_);
}

void LineMatcher::feed(const std::uint8_t* data, std::size_t len, std::vector<MatchEvent>& out) {
    const char* p = reinterpret_cast<const char*>(data);
    std::size_t i = 0;
    while (i < len) {
        if (dropping_) {
            const void* nl = std::memchr(p + i, '\n', len - i);
            if (nl == nullptr)
                return;
            auto nl_off = static_cast<std::size_t>(static_cast<const char*>(nl) - p);
            ++line_no_;
            dropping_ = false;
            i = nl_off + 1;
            continue;
        }

        if (!carry_.empty()) {
            const void* nl = std::memchr(p + i, '\n', len - i);
            if (nl == nullptr) {
                std::size_t rest = len - i;
                if (carry_.size() + rest <= max_line_bytes_) {
                    carry_.append(p + i, rest);
                } else {
                    dropped_long_ = true;
                    dropping_ = true;
                    carry_.clear();
                }
                return;
            }
            auto nl_off = static_cast<std::size_t>(static_cast<const char*>(nl) - p);
            std::size_t piece = nl_off - i;
            ++line_no_;
            if (carry_.size() + piece <= max_line_bytes_) {
                carry_.append(p + i, piece);
                if (line_contains(carry_, pattern_))
                    out.push_back(MatchEvent{line_no_, carry_});
            } else {
                dropped_long_ = true;
            }
            carry_.clear();
            i = nl_off + 1;
            continue;
        }

        const void* last = memrchr(p + i, '\n', len - i);
        if (last == nullptr) {
            std::size_t rest = len - i;
            if (rest > max_line_bytes_) {
                dropped_long_ = true;
                dropping_ = true;
            } else {
                carry_.append(p + i, rest);
            }
            return;
        }
        auto end = static_cast<std::size_t>(static_cast<const char*>(last) - p) + 1;
        scan_complete_region(p, i, end, out);
        i = end;
    }
}

void LineMatcher::finish(std::vector<MatchEvent>& out) {
    if (dropping_) {
        ++line_no_;
        dropping_ = false;
        carry_.clear();
        return;
    }
    if (!carry_.empty()) {
        ++line_no_;
        if (line_contains(carry_, pattern_))
            out.push_back(MatchEvent{line_no_, carry_});
        carry_.clear();
    }
}

void LineMatcher::scan_complete_region(const char* p, std::size_t i, std::size_t end,
                                       std::vector<MatchEvent>& out) {
    const std::size_t region_len = end - i;

    if (region_len > max_line_bytes_) {
        std::size_t floor = i;
        for (std::size_t s = i; s < end; s += max_line_bytes_) {
            const void* prev = memrchr(p + floor, '\n', s - floor);
            std::size_t ls = prev == nullptr
                                 ? floor
                                 : static_cast<std::size_t>(static_cast<const char*>(prev) - p) + 1;
            floor = ls;
            const void* nxt = std::memchr(p + s, '\n', end - s);
            std::size_t le = nxt == nullptr
                                 ? end - 1
                                 : static_cast<std::size_t>(static_cast<const char*>(nxt) - p);
            if (le - ls > max_line_bytes_)
                dropped_long_ = true;
        }
    }

    if (pattern_.empty()) {
        std::size_t ls = i;
        while (ls < end) {
            const void* nl = std::memchr(p + ls, '\n', end - ls);
            auto le = static_cast<std::size_t>(static_cast<const char*>(nl) - p);
            ++line_no_;
            if (le - ls <= max_line_bytes_) {
                out.push_back(MatchEvent{line_no_, std::string(p + ls, le - ls)});
            } else {
                dropped_long_ = true;
            }
            ls = le + 1;
        }
        return;
    }

    if (pattern_has_nl_) {
        line_no_ += count_newlines(p + i, region_len);
        return;
    }

    const std::string_view region(p + i, region_len);
    std::size_t frontier = i;
    std::uint64_t frontier_nl = 0;
    std::size_t cur_ls = i, cur_le = 0;
    bool cursor_valid = false;
    std::size_t hit = fast_find(region, 0, pattern_, anchor_off_);
    std::size_t last_emitted_ls = SIZE_MAX;
    while (hit != std::string_view::npos) {
        std::size_t a = i + hit;
        if (!cursor_valid) {
            cur_ls = i;
            const void* nl = std::memchr(p + cur_ls, '\n', end - cur_ls);
            cur_le = static_cast<std::size_t>(static_cast<const char*>(nl) - p);
            cursor_valid = true;
        }
        while (a > cur_le) {
            if (a - cur_le > 256) {
                const void* prev = memrchr(p + cur_le + 1, '\n', a - cur_le - 1);
                cur_ls = prev == nullptr
                             ? cur_le + 1
                             : static_cast<std::size_t>(static_cast<const char*>(prev) - p) + 1;
                frontier_nl += count_newlines(p + frontier, cur_ls - frontier);
                frontier = cur_ls;
                const void* nl = std::memchr(p + a, '\n', end - a);
                cur_le = static_cast<std::size_t>(static_cast<const char*>(nl) - p);
                break;
            }

            ++frontier_nl;
            cur_ls = cur_le + 1;
            frontier = cur_ls;
            const void* nl = std::memchr(p + cur_ls, '\n', end - cur_ls);
            cur_le = static_cast<std::size_t>(static_cast<const char*>(nl) - p);
        }
        const std::uint64_t line_no = line_no_ + 1 + frontier_nl;
        if (cur_ls != last_emitted_ls) {
            if (cur_le - cur_ls <= max_line_bytes_) {
                out.push_back(MatchEvent{line_no, std::string(p + cur_ls, cur_le - cur_ls)});
                last_emitted_ls = cur_ls;
            } else {
                dropped_long_ = true;
                last_emitted_ls = cur_ls;
            }
        }

        hit = fast_find(region, cur_le + 1 - i, pattern_, anchor_off_);
    }

    line_no_ += frontier_nl + count_newlines(p + frontier, end - frontier);
}

} // namespace sluice_grep
