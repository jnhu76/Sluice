// Grep performance-attribution workloads (deterministic, seeded).
//
// One generator serves every grep benchmark surface: the in-memory ladder
// (grep_attribution_bench), the on-disk file generator (grep_workload_gen,
// used for CLI / GNU grep / ripgrep comparisons), and the runner script —
// the same seed always produces byte-identical data, so a stage measured
// in-memory and the same stage measured through the CLI scan the exact
// same bytes.
//
// Design notes:
// - splitmix64 PRNG: stable across platforms/libstdc++ versions, no
//   <random> distribution objects whose semantics may drift.
// - Text topologies are built from a fixed English word list so byte
//   frequencies are realistic (e/t/a/o/s common, q/x/z rare) — pattern
//   choice (rare vs common anchor byte) then has a real effect on
//   candidate-scan performance, which is exactly what the workload matrix
//   must expose.
// - The pattern is EMBEDDED by overwriting bytes at a random offset of
//   match_frac of lines (never inserted): line lengths stay determined by
//   the topology, and a 0-density file contains zero occurrences.
// - '\n' occurs ONLY as a line terminator in generated data. Patterns used
//   with these workloads must not contain '\n' (a '\n' pattern never
//   matches line-oriented data; the differential test covers it separately).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sluice::bench::grep_wl {

// splitmix64 — 1 state word, full period, deterministic everywhere.
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // Uniform in [0, bound) for small bound (word picks / lengths).
    std::uint64_t below(std::uint64_t bound) { return next() % bound; }
};

enum class Topo { short_lines, normal_text, long_lines, huge_lines, binary };

struct Workload {
    std::string name;      // stable identifier used in CSV/JSON output
    std::string pattern;   // needle embedded at `match_frac` of lines
    Topo topo = Topo::normal_text;
    double match_frac = 0.0;  // fraction of lines containing the pattern
    std::uint64_t seed = 1;
    std::size_t target_bytes = 64ULL << 20;  // approximate output size
};

namespace detail {

inline constexpr std::string_view kWords[] = {
    "the",  "of",    "and",  "a",   "to",    "in",   "is",  "you",  "that",
    "it",   "he",    "was",  "for", "on",    "are",  "as",  "with", "his",
    "they", "at",    "be",   "this", "have", "from", "or",  "one",  "had",
    "by",   "word",  "but",  "not", "what",  "all",  "were", "we",  "when",
    "your", "can",   "said", "there", "use", "an",   "each", "which",
    "she",  "do",    "how",  "their", "if",  "will", "up",   "other", "about",
    "out",  "many",  "then", "them", "these", "so",  "some", "her",  "would",
    "make", "like",  "him",  "into", "time",  "has",  "look", "two",  "more",
};

// Approximate line-length ranges per topology (bytes, excluding '\n').
struct LineLen {
    std::size_t min, max;
};

inline LineLen topo_line_len(Topo t) {
    switch (t) {
        case Topo::short_lines: return {8, 20};
        case Topo::normal_text: return {36, 90};
        case Topo::long_lines: return {2 << 10, 6 << 10};
        case Topo::huge_lines: return {200 << 10, 240 << 10};  // near default cap
        case Topo::binary: return {24, 80};
    }
    return {36, 90};
}

// Fill one line body of `len` bytes (no '\n') with topology-appropriate bytes.
inline void fill_line_body(std::string& line, std::size_t len, Topo topo,
                           Rng& rng) {
    line.clear();
    line.reserve(len + 16);
    if (topo == Topo::binary) {
        // Any byte value except '\n' (0x0A): NULs, highs, controls included.
        while (line.size() < len) {
            std::uint64_t w = rng.next();
            for (int b = 0; b < 8 && line.size() < len; ++b) {
                unsigned char c = static_cast<unsigned char>(w >> (b * 8));
                if (c == '\n') c = 0x00;
                line.push_back(static_cast<char>(c));
            }
        }
        return;
    }
    // Word-based text for all text topologies.
    while (line.size() < len) {
        std::string_view w = kWords[rng.below(std::size(kWords))];
        if (!line.empty()) line.push_back(' ');
        line.append(w);
    }
    line.resize(len);
}

}  // namespace detail

// Generate the workload's full data (always '\n'-terminated lines; the
// final line is terminated too — the unterminated-final-line case is a
// semantics test concern, not a performance-shape concern).
inline std::string generate(const Workload& w) {
    using namespace detail;
    Rng rng{w.seed};
    std::string data;
    data.reserve(w.target_bytes + (240 << 10) + 64);
    const LineLen ll = topo_line_len(w.topo);
    std::string line;
    while (data.size() < w.target_bytes) {
        std::size_t len = ll.min + static_cast<std::size_t>(
                                        rng.below(ll.max - ll.min + 1));
        fill_line_body(line, len, w.topo, rng);
        if (!w.pattern.empty() && w.pattern.find('\n') == std::string::npos &&
            line.size() >= w.pattern.size() &&
            (w.match_frac >= 1.0 ||
             rng.below(1000000) <
                 static_cast<std::uint64_t>(w.match_frac * 1000000.0))) {
            std::size_t off = static_cast<std::size_t>(
                rng.below(line.size() - w.pattern.size() + 1));
            line.replace(off, w.pattern.size(), w.pattern);
        }
        line.push_back('\n');
        data.append(line);
    }
    return data;
}

// The fixed workload matrix for the attribution ladder: pattern classes ×
// densities × topologies (docs/verification/performance-attribution.md §C).
// Names are stable identifiers: <topo>__p_<pattern-tag>__d_<density>.
inline std::vector<Workload> matrix(std::size_t target_bytes) {
    auto mk = [=](std::string name, std::string pattern, Topo topo,
                  double frac) {
        Workload w;
        w.name = std::move(name);
        w.pattern = std::move(pattern);
        w.topo = topo;
        w.match_frac = frac;
        // FNV-1a over the name: seed is stable and distinct per workload.
        std::uint64_t h = 0xCBF29CE484222325ULL;
        for (char c : w.name) {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001B3ULL;
        }
        w.seed = h;
        w.target_bytes = target_bytes;
        return w;
    };
    std::vector<Workload> out;
    // Sparse realistic cases (the grep default shape): rare and common
    // anchors, short and medium patterns, over normal text.
    out.push_back(mk("normal__p_qz9__d_0", "qz9", Topo::normal_text, 0.0));
    out.push_back(mk("normal__p_qz9__d_s", "qz9", Topo::normal_text, 1e-6));
    out.push_back(mk("normal__p_the__d_0", "the", Topo::normal_text, 0.0));
    out.push_back(mk("normal__p_the__d_s", "the", Topo::normal_text, 1e-6));
    out.push_back(mk("normal__p_1b_e__d_0", "e", Topo::normal_text, 0.0));
    out.push_back(mk("normal__p_1b_z__d_s", "z", Topo::normal_text, 1e-6));
    out.push_back(mk("normal__p_16b__d_s", "QuiCk_br0wn_f0x!", Topo::normal_text, 1e-6));
    out.push_back(mk("normal__p_64b__d_s",
                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcd",
                     Topo::normal_text, 1e-6));
    out.push_back(mk("normal__p_rare1st__d_s", "\xC7q-vortex-marker", Topo::normal_text, 1e-6));
    out.push_back(mk("normal__p_rep__d_s", "abababab", Topo::normal_text, 1e-6));
    // Densities over a fixed mid pattern.
    out.push_back(mk("normal__p_the__d_1pc", "the", Topo::normal_text, 0.01));
    out.push_back(mk("normal__p_the__d_10pc", "the", Topo::normal_text, 0.10));
    out.push_back(mk("normal__p_the__d_all", "the", Topo::normal_text, 1.0));
    // Topologies at the sparse default.
    out.push_back(mk("short__p_the__d_s", "the", Topo::short_lines, 1e-6));
    out.push_back(mk("long__p_the__d_s", "the", Topo::long_lines, 1e-6));
    out.push_back(mk("huge__p_the__d_s", "the", Topo::huge_lines, 1e-6));
    out.push_back(mk("binary__p_the__d_s", "the", Topo::binary, 1e-6));
    return out;
}

}  // namespace sluice::bench::grep_wl
