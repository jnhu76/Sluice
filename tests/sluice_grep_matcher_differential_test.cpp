// sluice-grep matcher differential oracle: the optimized LineMatcher (V2,
// chunk-level occurrence scan) against the frozen V1 reference algorithm
// (split every line, std::search per line) on randomized inputs.
//
// This is the semantics-freeze proof for the performance-attribution
// optimization round (docs/verification/performance-attribution.md): V2 may
// change HOW lines are found, never WHAT is emitted. Randomized dimensions:
// data bytes (small alphabets stress overlapping/adversarial matches),
// patterns (empty, 1-byte, rare bytes, '\n'-containing, longer-than-line),
// chunk sizes 1..64 (cross-chunk everything), and max_line_bytes (forces
// every long-line drop path).
#include "harness.hpp"

#include "matcher.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace sluice_grep;

namespace {

// --- V1 reference: verbatim shape of the pre-optimization matcher (per-line
// std::search). Kept here as the oracle; do not "optimize" it.
struct RefMatcher {
    std::string pattern;
    std::size_t max_line_bytes;
    std::string carry;
    std::uint64_t line_no = 0;
    bool dropping = false;
    bool dropped_long = false;

    static bool contains(std::string_view line, std::string_view pat) {
        if (pat.empty()) return true;
        if (pat.size() > line.size()) return false;
        return std::search(line.begin(), line.end(), pat.begin(),
                           pat.end()) != line.end();
    }

    void feed(const std::uint8_t* data, std::size_t len,
              std::vector<MatchEvent>& out) {
        std::size_t i = 0;
        while (i < len) {
            if (dropping) {
                const void* nl = std::memchr(data + i, '\n', len - i);
                if (nl == nullptr) return;
                std::size_t nl_off =
                    static_cast<const std::uint8_t*>(nl) - data;
                ++line_no;
                dropping = false;
                i = nl_off + 1;
                continue;
            }
            const void* nl = std::memchr(data + i, '\n', len - i);
            if (nl == nullptr) {
                std::size_t rest = len - i;
                if (carry.size() + rest <= max_line_bytes) {
                    carry.append(reinterpret_cast<const char*>(data + i),
                                 rest);
                } else {
                    dropped_long = true;
                    dropping = true;
                    carry.clear();
                }
                return;
            }
            std::size_t nl_off =
                static_cast<const std::uint8_t*>(nl) - data;
            std::size_t piece = nl_off - i;
            if (!carry.empty()) {
                if (carry.size() + piece <= max_line_bytes) {
                    carry.append(reinterpret_cast<const char*>(data + i),
                                 piece);
                    ++line_no;
                    if (contains(carry, pattern))
                        out.push_back(MatchEvent{line_no, carry});
                    carry.clear();
                } else {
                    ++line_no;
                    dropped_long = true;
                    dropping = false;
                    carry.clear();
                }
                i = nl_off + 1;
            } else if (piece <= max_line_bytes) {
                ++line_no;
                if (piece == 0) {
                    if (pattern.empty()) out.push_back(MatchEvent{line_no, ""});
                } else {
                    std::string_view line(
                        reinterpret_cast<const char*>(data + i), piece);
                    if (contains(line, pattern))
                        out.push_back(MatchEvent{line_no, std::string(line)});
                }
                i = nl_off + 1;
            } else {
                ++line_no;
                dropped_long = true;
                carry.clear();
                i = nl_off + 1;
            }
        }
    }

    void finish(std::vector<MatchEvent>& out) {
        if (dropping) {
            ++line_no;
            dropping = false;
            carry.clear();
            return;
        }
        if (!carry.empty()) {
            ++line_no;
            if (contains(carry, pattern))
                out.push_back(MatchEvent{line_no, carry});
            carry.clear();
        }
    }
};

// Deterministic PRNG (splitmix64) so a failure reproduces from the seed.
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t b) { return next() % b; }
};

bool run_one_case(const std::string& pattern, const std::string& data,
                  std::size_t chunk, std::size_t cap, std::string* err) {
    std::vector<MatchEvent> got, want;
    LineMatcher m(pattern, cap);
    RefMatcher r{pattern, cap, {}, 0, false, false};
    for (std::size_t i = 0; i < data.size(); i += chunk) {
        std::size_t n = std::min(chunk, data.size() - i);
        m.feed(reinterpret_cast<const std::uint8_t*>(data.data()) + i, n,
               got);
        r.feed(reinterpret_cast<const std::uint8_t*>(data.data()) + i, n,
               want);
    }
    m.finish(got);
    r.finish(want);

    if (got.size() != want.size()) {
        *err = "match count " + std::to_string(got.size()) + " != " +
               std::to_string(want.size());
        return false;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i].line_no != want[i].line_no || got[i].line != want[i].line) {
            *err = "event " + std::to_string(i) + ": (" +
                   std::to_string(got[i].line_no) + ",'" + got[i].line +
                   "') != (" + std::to_string(want[i].line_no) + ",'" +
                   want[i].line + "')";
            return false;
        }
    }
    if (m.complete_lines() != r.line_no) {
        *err = "complete_lines " + std::to_string(m.complete_lines()) +
               " != " + std::to_string(r.line_no);
        return false;
    }
    if (m.dropped_long_lines() != r.dropped_long) {
        *err = std::string("dropped_long ") +
               (m.dropped_long_lines() ? "1" : "0") + " != " +
               (r.dropped_long ? "1" : "0");
        return false;
    }
    return true;
}

std::string gen_data(Rng& rng, int style) {
    // style 0: tiny alphabet dense overlaps; 1: ascii text-ish; 2: includes
    // high/control bytes and NULs; 3: long runs without newlines.
    static const char* alphabets[] = {"aab\n", "abcxyz \n", nullptr, "aa\n"};
    std::string d;
    std::size_t target = 1 + rng.below(300);
    while (d.size() < target) {
        if (style == 1) {
            const char* words[] = {"ab",  "abc", "xy",  "bcx", "zq",
                                   "aab", "the", "\n"};
            d.append(words[rng.below(8)]);
            if (rng.below(4) == 0) d.push_back(' ');
        } else if (style == 2) {
            std::uint64_t w = rng.next();
            for (int b = 0; b < 8 && d.size() < target; ++b) {
                unsigned char c = static_cast<unsigned char>(w >> (b * 8));
                if (rng.below(8) == 0) c = '\n';
                d.push_back(static_cast<char>(c));
            }
        } else {
            d.push_back(alphabets[style == 0 ? 0 : 3]
                                    [rng.below(style == 0 ? 4 : 3)]);
        }
    }
    return d;
}

std::string gen_pattern(Rng& rng) {
    static const char* pats[] = {
        "",   "a",    "aa",     "ab",  "abc", "aab", "aaab", "banana",
        "the", "xy", "zqx", "\xC7q", "aa\nb", "\n", "abcabcabcabcabcabcabc",
        "this-pattern-is-longer-than-most-lines-in-the-pool",
        "aaaa",
    };
    return pats[rng.below(17)];
}

}  // namespace

SLUICE_TEST_CASE(matcher_differential_randomized) {
    Rng rng{0x511CEFA11ULL};
    int cases = 0;
    for (int iter = 0; iter < 4000; ++iter) {
        std::string pat = gen_pattern(rng);
        std::string data = gen_data(rng, static_cast<int>(rng.below(4)));
        std::size_t chunk = 1 + rng.below(64);
        std::size_t cap = 1 + rng.below(24);  // small caps exercise drops
        std::string err;
        if (!run_one_case(pat, data, chunk, cap, &err)) {
            SLUICE_CHECK_MSG(false,
                             "seed-iter " + std::to_string(iter) + " pat='" +
                                 pat + "' chunk=" + std::to_string(chunk) +
                                 " cap=" + std::to_string(cap) + ": " + err);
            return;
        }
        ++cases;
    }
    SLUICE_CHECK_MSG(cases == 4000, "case count");
}

SLUICE_TEST_CASE(matcher_differential_large_cap_chunk_sweep) {
    // The same inputs under a large cap: no drops, only cross-chunk assembly
    // and matching. Chunk sizes straddle every boundary of every pattern.
    Rng rng{0xD1FFE5EEDULL};
    for (int iter = 0; iter < 400; ++iter) {
        std::string pat = gen_pattern(rng);
        std::string data = gen_data(rng, static_cast<int>(rng.below(4)));
        for (std::size_t chunk : {std::size_t(1), std::size_t(2),
                                  std::size_t(3), std::size_t(7),
                                  std::size_t(16), std::size_t(1000)}) {
            std::string err;
            if (!run_one_case(pat, data, chunk, 1 << 20, &err)) {
                SLUICE_CHECK_MSG(false, "iter " + std::to_string(iter) +
                                            " chunk=" +
                                            std::to_string(chunk) + ": " + err);
                return;
            }
        }
    }
    SLUICE_CHECK(true);
}

SLUICE_TEST_CASE(matcher_differential_boundary_fixed_cases) {
    // Hand-picked adversarial shapes the random loop may under-sample.
    struct Case {
        const char* pat;
        std::string data;
        std::size_t cap;
    };
    std::vector<Case> cases = {
        {"aa", "aaaa\n", 8},                     // overlapping matches, one line
        {"aa", "aa\naa\naa\n", 8},               // one per line
        {"aa", "aaa\nbaaab\n", 8},               // multiple hits per line
        {"a\nb", "xa\nbx\n", 8},                 // pattern spans a newline
        {"\n", "abc\n\nxyz\n", 8},               // newline-only pattern
        {"", "a\n\nb\n", 8},                     // empty pattern, empty line
        {"abc", "xxab", 8},                      // unterminated tail, no match
        {"abc", "xxabc", 8},                     // unterminated tail, match at finish
        {"q", std::string(100, 'q') + "\n", 8},  // long-line drop, match inside
        {"q", std::string(8, 'q') + "\nq\n", 8}, // line exactly at cap
        {"q", std::string(9, 'q') + "\n", 8},    // one over cap
        {"\xC7", "\xC7\xC7\n\xC7", 8},           // rare-byte anchor
        {"abab", "abababab\n", 8},               // repetitive pattern
        {"aab", "aaaaaab\n", 8},                 // candidates then hit
    };
    for (auto& c : cases) {
        for (std::size_t chunk = 1; chunk <= c.data.size() + 1; ++chunk) {
            std::string err;
            if (!run_one_case(c.pat, c.data, chunk, c.cap, &err)) {
                SLUICE_CHECK_MSG(false, std::string("pat='") + c.pat +
                                            "' chunk=" +
                                            std::to_string(chunk) + ": " + err);
                return;
            }
        }
    }
    SLUICE_CHECK(true);
}

SLUICE_MAIN()
