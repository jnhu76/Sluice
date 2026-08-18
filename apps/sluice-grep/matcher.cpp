// sluice-grep — streaming literal line matcher implementation.
//
// Platform scope: deliberately Linux/glibc. The fast-scan paths use GNU
// extensions (`memrchr`, guarded by `_GNU_SOURCE`; the bench ladder also
// uses `memmem`) that are NOT POSIX — per POSIX.1-2008 only forward
// `memchr` is standard. Sluice's CI and application track are Linux-only
// today, so this dependency is accepted and documented here rather than
// papered over with a portable-but-slower fallback; a non-glibc port would
// substitute a reverse newline scan and re-run the differential oracle
// (tests/sluice_grep_matcher_differential_test.cpp). Do not silently widen
// or narrow this scope.
//
// V2 attribution-driven scan (docs/verification/performance-attribution.md):
// the V1 shape (split every line, then std::search each line) pays a
// per-byte quadratic-ish scan and re-derives search state per line. V2 scans
// each chunk's COMPLETE-line region once for pattern occurrences
// (anchor-byte memchr + memcmp verify — O(region) with SIMD memchr and
// ~zero cost per non-candidate byte), and resolves the enclosing line of a
// hit only when a hit exists (memrchr back + memchr forward). Line numbers
// come from an incremental newline-count frontier, so the whole chunk is
// examined for '\n' exactly once per feed. Semantics are byte-identical to
// V1 (frozen in matcher.hpp; proven by the differential test
// tests/sluice_grep_matcher_differential_test.cpp against the V1 reference
// implementation).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "matcher.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace sluice_grep {

namespace {

// Rough English-text frequency score for anchor selection; lower =
// expected rarer in text-like input (finer tiers than a plain letter set:
// which byte anchors "the" decides its candidate density). A bad estimate
// is only a performance issue (more false candidates to memcmp away),
// never a correctness issue.
inline int byte_freq_score(unsigned char c) {
    switch (c) {
        case ' ': return 10;
        case 'e': case 'E': return 9;
        case 't': case 'T': return 8;
        case 'a': case 'A': case 'o': case 'O': return 7;
        case 'i': case 'I': case 'n': case 'N': return 6;
        case 's': case 'S': case 'h': case 'H': return 5;
        case 'r': case 'R': return 4;
        case 'd': case 'l': case 'u': case 'U': return 3;
        case 'c': case 'm': case 'w': case 'f': case 'g': case 'y':
        case 'p': case 'b': case 'C': case 'M': case 'W': case 'F':
        case 'G': case 'Y': case 'P': case 'B':
            return 2;
        case '\n': return 5;
        default:
            if (c >= '0' && c <= '9') return 2;
            if (c >= 'a' && c <= 'z') return 1;  // remaining lowercase
            if (c >= 'A' && c <= 'Z') return 1;
            if (c >= 0x80) return 0;
            if (c < 0x20) return 1;
            return 1;  // remaining punctuation
    }
}

// Count '\n' in [p, p+n) with a portable 4x8-byte SWAR pipeline. The
// byte==c test is the borrow-free form ~(v | ((v|H)-O)) & H with v =
// w^broadcast(c): every byte of (v|H) is >= 0x80, so subtracting 0x01 per
// byte never underflows and never borrows across bytes — the classic
// haszero form ((v-O)&~v&H) is NOT usable for counting because its
// subtraction borrows (e.g. "\n\x0B" overcounts). One linear pass with no
// per-occurrence libc call; reads stay in-bounds via memcpy word loads.
inline std::uint64_t count_newlines(const char* p, std::size_t n) {
    constexpr std::uint64_t kOnes = 0x0101010101010101ULL;
    constexpr std::uint64_t kHighs = 0x8080808080808080ULL;
    constexpr std::uint64_t kXor = 0x0A0A0A0A0A0A0A0AULL;  // '\n' broadcast
    auto word_hits = [](std::uint64_t w) {
        w ^= kXor;  // matching bytes read as 0x00
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
    for (; i < n; ++i) count += (p[i] == '\n');
    return count;
}

// First occurrence of `pat` (non-empty) in hay[pos, hay.size()); npos if
// none. Anchor strategy: memchr for the pattern's rarest byte, memcmp the
// whole pattern at each candidate. Worst case is O(n*m) memcmps (peaks on
// adversarial repetitive data) but each rejection is one short SIMD memcmp.
std::size_t fast_find(std::string_view hay, std::size_t pos,
                      std::string_view pat, std::size_t anchor_off) {
    const std::size_t n = hay.size();
    if (pos + pat.size() > n) return std::string_view::npos;
    const unsigned char anchor = static_cast<unsigned char>(pat[anchor_off]);
    const char* base = hay.data();
    std::size_t p = pos;  // next candidate start
    while (p + pat.size() <= n) {
        // memchr may report the anchor no later than the last position
        // where a full pattern still fits (n - pat.size() + anchor_off).
        const void* hit = std::memchr(base + p + anchor_off, anchor,
                                      n - pat.size() - p + 1);
        if (hit == nullptr) return std::string_view::npos;
        std::size_t a = static_cast<std::size_t>(
                            static_cast<const char*>(hit) - base) -
                        anchor_off;
        if (std::memcmp(base + a, pat.data(), pat.size()) == 0) return a;
        p = a + 1;
    }
    return std::string_view::npos;
}

}  // namespace

// Literal substring test (std::search semantics; empty needle => true).
bool line_contains(std::string_view line, std::string_view pattern) {
    if (pattern.empty()) return true;
    if (pattern.size() > line.size()) return false;
    if (pattern.find('\n') != std::string_view::npos) return false;
    std::size_t anchor = 0;
    int best = byte_freq_score(static_cast<unsigned char>(pattern[0]));
    for (std::size_t i = 1; i < pattern.size(); ++i) {
        int sc = byte_freq_score(static_cast<unsigned char>(pattern[i]));
        if (sc < best) {
            best = sc;
            anchor = i;
        }
    }
    return fast_find(line, 0, pattern, anchor) != std::string_view::npos;
}

LineMatcher::LineMatcher(std::string pattern, std::size_t max_line_bytes)
    : pattern_(std::move(pattern)),
      max_line_bytes_(max_line_bytes),
      anchor_off_(0),
      pattern_has_nl_(pattern_.find('\n') != std::string::npos) {
    // Reserve the carry's full budget up front so steady-state feeding never
    // allocates (the buffer only grows up to max_line_bytes_).
    carry_.reserve(max_line_bytes_ + 1);
    if (!pattern_.empty()) {
        int best = byte_freq_score(static_cast<unsigned char>(pattern_[0]));
        for (std::size_t i = 1; i < pattern_.size(); ++i) {
            int sc =
                byte_freq_score(static_cast<unsigned char>(pattern_[i]));
            if (sc < best) {
                best = sc;
                anchor_off_ = i;
            }
        }
    }
}

void LineMatcher::feed(const std::uint8_t* data, std::size_t len,
                       std::vector<MatchEvent>& out) {
    const char* p = reinterpret_cast<const char*>(data);
    std::size_t i = 0;
    while (i < len) {
        if (dropping_) {
            // Inside a too-long unterminated line: discard up to and
            // including the next newline, then resume normal assembly.
            const void* nl = std::memchr(p + i, '\n', len - i);
            if (nl == nullptr) return;  // still inside the long line
            auto nl_off = static_cast<std::size_t>(
                              static_cast<const char*>(nl) - p);
            ++line_no_;
            dropping_ = false;
            i = nl_off + 1;
            continue;
        }

        if (!carry_.empty()) {
            // A line from a previous chunk is pending: complete it first so
            // cross-chunk patterns/lines stay findable (the completed line
            // is searched as one unit).
            const void* nl = std::memchr(p + i, '\n', len - i);
            if (nl == nullptr) {
                std::size_t rest = len - i;
                if (carry_.size() + rest <= max_line_bytes_) {
                    carry_.append(p + i, rest);
                } else {
                    // Over cap while still unterminated: drop and discard
                    // the remainder of this line.
                    dropped_long_ = true;
                    dropping_ = true;
                    carry_.clear();
                }
                return;
            }
            auto nl_off =
                static_cast<std::size_t>(static_cast<const char*>(nl) - p);
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

        // Fast path: carry empty, i is a line boundary. Find the last
        // complete line in this chunk; [i, end) is all '\n'-terminated.
        const void* last = memrchr(p + i, '\n', len - i);
        if (last == nullptr) {
            // No complete line in the rest of the chunk: park the bytes in
            // the carry (or enter dropping mode when already over cap).
            std::size_t rest = len - i;
            if (rest > max_line_bytes_) {
                dropped_long_ = true;
                dropping_ = true;
            } else {
                carry_.append(p + i, rest);
            }
            return;
        }
        auto end =
            static_cast<std::size_t>(static_cast<const char*>(last) - p) + 1;
        scan_complete_region(p, i, end, out);
        i = end;
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

// [i, end) consists solely of complete '\n'-terminated lines; i is a line
// boundary; the carry is empty. Three shapes:
//  - empty pattern: every line matches;
//  - pattern containing '\n': nothing can match (one line never contains a
//    newline) — only line accounting runs;
//  - ordinary pattern: one chunk-wide occurrence scan; each hit resolves
//    its enclosing line (memrchr back, memchr forward) and line number
//    (incremental '\n' frontier — the region is examined for newlines at
//    most once across all hits).
void LineMatcher::scan_complete_region(const char* p, std::size_t i,
                                       std::size_t end,
                                       std::vector<MatchEvent>& out) {
    const std::size_t region_len = end - i;

    // Over-cap complete lines must be flagged (dropped_long_) wherever they
    // sit. A sample point every max_line_bytes_ bytes provably lands inside
    // every line longer than the cap (any M+1 consecutive byte offsets
    // contain a multiple of M), so 2 libc calls per cap-window replace a
    // full per-line length walk. Skipped entirely when no complete line can
    // exceed the cap.
    if (region_len > max_line_bytes_) {
        for (std::size_t s = i; s < end; s += max_line_bytes_) {
            const void* prev = memrchr(p + i, '\n', s - i);
            std::size_t ls =
                prev == nullptr
                    ? i
                    : static_cast<std::size_t>(
                          static_cast<const char*>(prev) - p) + 1;
            const void* nxt = std::memchr(p + s, '\n', end - s);
            std::size_t le =
                nxt == nullptr
                    ? end - 1
                    : static_cast<std::size_t>(
                          static_cast<const char*>(nxt) - p);
            if (le - ls > max_line_bytes_) dropped_long_ = true;
        }
    }

    if (pattern_.empty()) {
        // Every line matches, in order, with the cap policy applied.
        std::size_t ls = i;
        while (ls < end) {
            const void* nl = std::memchr(p + ls, '\n', end - ls);
            auto le = static_cast<std::size_t>(
                static_cast<const char*>(nl) - p);
            ++line_no_;
            if (le - ls <= max_line_bytes_) {
                out.push_back(MatchEvent{line_no_,
                                         std::string(p + ls, le - ls)});
            } else {
                dropped_long_ = true;
            }
            ls = le + 1;
        }
        return;
    }

    if (pattern_has_nl_) {
        // Cannot match (a line never contains '\n'); still account lines.
        line_no_ += count_newlines(p + i, region_len);
        return;
    }

    // Ordinary pattern: chunk-wide occurrence scan + incremental line
    // cursor. The cursor (cur_ls, cur_le) tracks the line containing the
    // most recent occurrence; a new occurrence in the same line costs
    // nothing, a near miss walks lines forward one memchr each (dense-match
    // shape), and a far miss JUMPS with one memrchr/memchr pair instead of
    // walking thousands of lines (sparse-match shape — a per-occurrence
    // unbounded line rescan here is what made huge-line workloads
    // quadratic). The newline frontier RIDES the cursor: each walk step
    // crosses exactly one newline, so only jump gaps and the final tail pay
    // the SWAR counter — dense rows are not counted twice.
    const std::string_view region(p + i, region_len);
    std::size_t frontier = i;      // '\n' count is exact up to here (exclusive)
    std::uint64_t frontier_nl = 0; // newlines in [i, frontier)
    std::size_t cur_ls = i, cur_le = 0;
    bool cursor_valid = false;
    std::size_t hit = fast_find(region, 0, pattern_, anchor_off_);
    std::size_t last_emitted_ls = SIZE_MAX;
    while (hit != std::string_view::npos) {
        std::size_t a = i + hit;  // absolute occurrence offset
        if (!cursor_valid) {
            cur_ls = i;
            const void* nl = std::memchr(p + cur_ls, '\n', end - cur_ls);
            cur_le = static_cast<std::size_t>(
                         static_cast<const char*>(nl) - p);
            cursor_valid = true;
        }
        while (a > cur_le) {  // occurrence lies in a later line
            if (a - cur_le > 256) {
                // Sparse gap: jump straight to the enclosing line and SWAR
                // only the skipped bytes.
                const void* prev =
                    memrchr(p + cur_le + 1, '\n', a - cur_le - 1);
                cur_ls = prev == nullptr
                             ? cur_le + 1
                             : static_cast<std::size_t>(
                                   static_cast<const char*>(prev) - p) + 1;
                frontier_nl += count_newlines(p + frontier, cur_ls - frontier);
                frontier = cur_ls;
                const void* nl = std::memchr(p + a, '\n', end - a);
                cur_le = static_cast<std::size_t>(
                             static_cast<const char*>(nl) - p);
                break;
            }
            // Walk one line forward: crossing exactly its terminator.
            ++frontier_nl;
            cur_ls = cur_le + 1;
            frontier = cur_ls;
            const void* nl = std::memchr(p + cur_ls, '\n', end - cur_ls);
            cur_le = static_cast<std::size_t>(
                         static_cast<const char*>(nl) - p);
        }
        const std::uint64_t line_no = line_no_ + 1 + frontier_nl;
        if (cur_ls != last_emitted_ls) {  // one MatchEvent per line
            if (cur_le - cur_ls <= max_line_bytes_) {
                out.push_back(
                    MatchEvent{line_no, std::string(p + cur_ls, cur_le - cur_ls)});
                last_emitted_ls = cur_ls;
            } else {
                dropped_long_ = true;
                last_emitted_ls = cur_ls;  // dropped lines also count once
            }
        }
        // Additional occurrences in the same line cannot change its outcome:
        // resume the scan past this line's terminator. (This is what keeps
        // dense-match workloads from paying for every repeat occurrence.)
        hit = fast_find(region, cur_le + 1 - i, pattern_, anchor_off_);
    }
    // Account the remainder for the exact total line count of the region.
    line_no_ += frontier_nl + count_newlines(p + frontier, end - frontier);
}

}  // namespace sluice_grep
