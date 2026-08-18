// Grep performance-attribution ladder (docs/verification/
// performance-attribution.md). One binary measures every layer of the
// sluice-grep pipeline over the SAME deterministic workload bytes, so
// Cost(Ln) - Cost(Ln-1) isolates what layer n adds:
//
//   L0_sum            byte-sum loop             (raw memory-read floor)
//   L0_memchr_nl      memchr('\n') count        (newline-scan kernel)
//   L1_line_std       split + std::search/line  (V1 matcher core shape)
//   L1_chunk_anchor   chunk anchor-memchr scan  (candidate-scan primitive)
//   L1_chunk_memmem   glibc memmem              (libc SIMD two-way ref)
//   L2_matcher        full LineMatcher + sink   (line assembly + events)
//   L2e_matcher_emit  L2 + formatted output     (+ fwrite/fputc cost)
//   L3_pread_matcher  pread + LineMatcher       (+ blocking file I/O)
//   L4_sluice         sluice_grep::grep_files   (+ Sluice Runtime/TP backend)
//
// L5 (alternative production backend) has no default-build candidate and is
// measured out of band; L6 (full CLI + stdout) is measured by the runner
// (scripts/bench/perf-attribution.py) against the real binary.
//
// CSV to stdout, one row per (stage, workload): min/median/max ns over the
// measured iterations plus a stage-defined `matches` cross-check value and
// the raw per-iteration samples in run order (`ns_samples`). The median is
// the true mathematical median (mean of the two middle samples for an even
// iteration count), not an upper-middle pick that silently biases even-iter
// runs. Results are environment-sensitive — no universal performance claim.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "grep_workloads.hpp"
#include "temp_path.hpp"

#include "grep_task.hpp"
#include "matcher.hpp"

#include <sluice/async/threadpool_backend.hpp>
#include <sluice/detail/posix_retry.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace sluice::bench;
using sluice_grep::LineMatcher;
using sluice_grep::MatchEvent;

namespace {

// Run-ending failure: a benchmark whose I/O or engine cross-check failed
// must abort loudly, never emit a sentinel row (a silent ~0ULL `matches`
// value or a skipped stage would corrupt the evidence).
[[noreturn]] void bench_fatal(const char* what, int err) {
    std::fprintf(stderr, "grep_attribution_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(3);
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// ---------------------------------------------------------------------------
// L1 candidate-scan primitive (bench-local: the strategy the matcher
// optimization evaluates BEFORE any production change. Anchor on the
// pattern byte with the lowest estimated text frequency, memchr for it,
// memcmp-verify the full pattern).
// ---------------------------------------------------------------------------

// Rough printable-ASCII English frequency score; lower = rarer in text.
inline int byte_freq_score(unsigned char c) {
    switch (c) {
        case ' ': case 'e': case 'E': case 't': case 'T': case 'a':
        case 'A': case 'o': case 'O': case 'n': case 'N': case 'i':
        case 'I': case 's': case 'S': case 'h': case 'H': case 'r':
        case 'R':
            return 9;
        case 'd': case 'l': case 'u': case 'c': case 'm': case 'w':
        case 'f': case 'g': case 'y': case 'p': case 'b':
            return 6;
        case '.': case ',': case '\n':
            return 5;
        default:
            if (c >= '0' && c <= '9') return 5;
            if (c >= 'a' && c <= 'z') return 3;  // remaining lowercase
            if (c >= 'A' && c <= 'Z') return 3;
            if (c >= 0x80) return 0;  // high bytes: rare in text workloads
            if (c < 0x20) return 1;   // control bytes (not \n): rare
            return 2;                 // remaining punctuation
    }
}

// Count verified occurrences of `pat` in `hay` (resume at hit+1).
std::uint64_t anchor_scan_count(std::string_view hay, std::string_view pat) {
    if (pat.empty()) return 0;  // caller handles the empty pattern shape
    std::size_t anchor_off = 0;
    int best = byte_freq_score(static_cast<unsigned char>(pat[0]));
    for (std::size_t i = 1; i < pat.size(); ++i) {
        int sc = byte_freq_score(static_cast<unsigned char>(pat[i]));
        if (sc < best) {
            best = sc;
            anchor_off = i;
        }
    }
    const unsigned char anchor = static_cast<unsigned char>(pat[anchor_off]);
    const std::size_t pat_len = pat.size();
    const char* base = hay.data();
    std::uint64_t hits = 0;
    std::size_t pos = 0;  // next candidate start
    while (pos + pat_len <= hay.size()) {
        // Last valid candidate starts at hay.size()-pat_len, so its anchor
        // byte is the last position memchr may report.
        const void* hit = std::memchr(base + pos + anchor_off, anchor,
                                      hay.size() - pat_len - pos + 1);
        if (hit == nullptr) break;
        std::size_t a = static_cast<std::size_t>(
                            static_cast<const char*>(hit) - base) -
                        anchor_off;
        if (std::memcmp(base + a, pat.data(), pat_len) == 0) ++hits;
        pos = a + 1;
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Stage implementations. Each returns its cross-check value (matches /
// occurrences / checksum — the metric is stage-defined).
// ---------------------------------------------------------------------------

std::uint64_t stage_l0_sum(const std::string& data, const std::string&,
                           std::size_t) {
    std::uint64_t sum = 0;
    for (char c : data) sum += static_cast<unsigned char>(c);
    return sum;
}

std::uint64_t stage_l0_memchr_nl(const std::string& data, const std::string&,
                                 std::size_t) {
    std::uint64_t lines = 0;
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const void* nl = std::memchr(p, '\n', left);
        if (nl == nullptr) break;
        ++lines;
        auto step =
            static_cast<std::size_t>(static_cast<const char*>(nl) - p) + 1;
        p += step;
        left -= step;
    }
    return lines;
}

// V1 matcher core shape: newline-split then per-line std::search.
std::uint64_t stage_l1_line_std(const std::string& data,
                                const std::string& pat, std::size_t) {
    std::uint64_t matched_lines = 0;
    std::size_t pos = 0;
    while (pos < data.size()) {
        const void* nl =
            std::memchr(data.data() + pos, '\n', data.size() - pos);
        std::size_t end =
            nl == nullptr
                ? data.size()
                : static_cast<std::size_t>(
                      static_cast<const char*>(nl) - data.data());
        std::string_view line(data.data() + pos, end - pos);
        if (pat.empty() ||
            std::search(line.begin(), line.end(), pat.begin(), pat.end()) !=
                line.end())
            ++matched_lines;
        pos = end + 1;
    }
    return matched_lines;
}

std::uint64_t stage_l1_chunk_anchor(const std::string& data,
                                    const std::string& pat, std::size_t) {
    return anchor_scan_count(data, pat);
}

std::uint64_t stage_l1_chunk_memmem(const std::string& data,
                                    const std::string& pat, std::size_t) {
    if (pat.empty()) return 0;
    std::uint64_t hits = 0;
    const char* cur = data.data();
    std::size_t left = data.size();
    while (left >= pat.size()) {
        const void* hit = ::memmem(cur, left, pat.data(), pat.size());
        if (hit == nullptr) break;
        ++hits;
        auto step =
            static_cast<std::size_t>(static_cast<const char*>(hit) - cur) + 1;
        cur += step;
        left -= step;
    }
    return hits;
}

// Full matcher over 1 MiB chunks with a counting sink (no output).
std::uint64_t run_matcher_count(const std::string& data,
                                const std::string& pat,
                                std::size_t max_line_bytes,
                                std::FILE* out) {
    LineMatcher m(pat, max_line_bytes);
    std::vector<MatchEvent> events;
    std::uint64_t count = 0;
    constexpr std::size_t kChunk = 1 << 20;
    auto drain = [&count, out](std::vector<MatchEvent>& ev) {
        for (auto& e : ev) {
            if (out != nullptr) {
                std::fwrite(e.line.data(), 1, e.line.size(), out);
                std::fputc('\n', out);
            }
            ++count;
        }
    };
    for (std::size_t i = 0; i < data.size(); i += kChunk) {
        std::size_t n = std::min(kChunk, data.size() - i);
        events.clear();
        m.feed(reinterpret_cast<const std::uint8_t*>(data.data()) + i, n,
               events);
        drain(events);
    }
    events.clear();
    m.finish(events);
    drain(events);
    return count;
}

std::uint64_t stage_l2_matcher(const std::string& data,
                               const std::string& pat,
                               std::size_t max_line_bytes) {
    return run_matcher_count(data, pat, max_line_bytes, nullptr);
}

std::uint64_t stage_l2e_matcher_emit(const std::string& data,
                                     const std::string& pat,
                                     std::size_t max_line_bytes) {
    // L2 + the CLI sink's formatting/write cost into a suppressed file
    // (the same stdio calls as apps/sluice-grep/main.cpp's sink).
    std::FILE* out = std::fopen("/dev/null", "w");
    if (out == nullptr) bench_fatal("L2e fopen(/dev/null)", errno);
    std::uint64_t n = run_matcher_count(data, pat, max_line_bytes, out);
    std::fclose(out);
    return n;
}

// L3: blocking positional reads + matcher (no Sluice).
std::uint64_t stage_l3_pread(const std::string& path, const std::string& pat,
                             std::size_t buffer_size,
                             std::size_t max_line_bytes) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) bench_fatal("L3 open workload file", errno);
    std::vector<std::uint8_t> buf(buffer_size);
    LineMatcher m(pat, max_line_bytes);
    std::vector<MatchEvent> events;
    std::uint64_t count = 0;
    std::uint64_t off = 0;
    for (;;) {
        // Repository retry authority for interrupted blocking syscalls
        // (include/sluice/detail/posix_retry.hpp); every other failure is
        // a run-ending error, not a silent sentinel.
        ssize_t n = sluice::detail::retry_on_eintr([&] {
            return ::pread(fd, buf.data(), buf.size(),
                           static_cast<off_t>(off));
        });
        if (n < 0) {
            int e = errno;
            ::close(fd);
            bench_fatal("L3 pread failed", e);
        }
        if (n == 0) break;
        events.clear();
        m.feed(buf.data(), static_cast<std::size_t>(n), events);
        count += events.size();
        off += static_cast<std::uint64_t>(n);
    }
    events.clear();
    m.finish(events);
    count += events.size();
    ::close(fd);
    return count;
}

// L4: the real engine — Sluice ApplicationRuntime + ThreadPoolBackend.
std::uint64_t stage_l4_sluice(const std::string& path, const std::string& pat,
                              std::size_t buffer_size,
                              std::size_t max_line_bytes) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) bench_fatal("L4 open workload file", errno);
    std::vector<sluice_grep::GrepInput> inputs;
    inputs.push_back(sluice_grep::GrepInput{path, fd});
    std::uint64_t count = 0;
    auto sink = [&count](const std::string&, std::uint64_t,
                         std::string_view) { ++count; };
    auto results = sluice_grep::grep_files(
        pat, std::move(inputs), buffer_size, max_line_bytes, 1, sink);
    ::close(fd);
    if (results.empty()) bench_fatal("L4 engine returned no result", 0);
    if (results[0].match_count != count)
        bench_fatal("L4 sink/engine match-count disagreement", 0);
    return count;
}

// ---------------------------------------------------------------------------

// True mathematical median of the sorted samples: middle element for odd n,
// mean of the two middle elements for even n (arbitrary --iters must not
// silently bias the reported median upward).
double median_sorted(const std::vector<std::uint64_t>& sorted) {
    const std::size_t n = sorted.size();
    if (n % 2 == 1) return static_cast<double>(sorted[n / 2]);
    return (static_cast<double>(sorted[n / 2 - 1]) +
            static_cast<double>(sorted[n / 2])) /
           2.0;
}

void emit_row(const std::string& stage, const std::string& wl,
              std::size_t bytes, const std::vector<std::uint64_t>& ns,
              std::uint64_t matches) {
    std::vector<std::uint64_t> sorted = ns;
    std::sort(sorted.begin(), sorted.end());
    const double med = median_sorted(sorted);
    // bytes/ns is GB/s (1 byte/ns == 1 GB/s).
    double gbps = static_cast<double>(bytes) / med;
    // Raw samples in run order (not sorted): evidence artifacts must preserve
    // the per-iteration distribution, not just the derived statistics.
    std::string samples;
    for (std::size_t i = 0; i < ns.size(); ++i) {
        if (i != 0) samples += ';';
        samples += std::to_string(ns[i]);
    }
    std::printf("%s,%s,%zu,%zu,%llu,%.1f,%llu,%.3f,%llu,%s\n", stage.c_str(),
                wl.c_str(), bytes, sorted.size(),
                (unsigned long long)sorted.front(), med,
                (unsigned long long)sorted.back(), gbps,
                (unsigned long long)matches, samples.c_str());
}

struct Config {
    std::size_t bytes = 64ULL << 20;
    std::size_t iters = 5;
    std::size_t warmup = 1;
    std::size_t buffer_size = 1 << 20;
    std::size_t max_line_bytes = 1 << 20;
    std::vector<std::string> stages;     // empty = all
    std::vector<std::string> workloads;  // empty = all
};

bool selected(const std::vector<std::string>& sel, const std::string& item) {
    return sel.empty() ||
           std::find(sel.begin(), sel.end(), item) != sel.end();
}

std::vector<std::string> split_list(const char* s) {
    std::vector<std::string> out;
    std::string list(s);
    std::size_t p = 0;
    while (p < list.size()) {
        std::size_t c = list.find(',', p);
        if (c == std::string::npos) c = list.size();
        if (c > p) out.emplace_back(list.substr(p, c - p));
        p = c + 1;
    }
    return out;
}

struct MemStage {
    const char* name;
    std::uint64_t (*fn)(const std::string&, const std::string&, std::size_t);
};
struct FileStage {
    const char* name;
    std::uint64_t (*fn)(const std::string&, const std::string&, std::size_t,
                        std::size_t);
};

}  // namespace

// Whole-number decimal parse: rejects empty text, trailing garbage, and
// overflow (strtoull would silently accept "5x", "-3", and wrap-arounds).
bool parse_size(const char* s, std::size_t& out) {
    if (s == nullptr || *s == '\0') return false;
    std::size_t v = 0;
    const std::size_t limit = std::numeric_limits<std::size_t>::max();
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned d = static_cast<unsigned>(*p - '0');
        if (v > (limit - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

int usage_error(const char* argv0, const char* detail) {
    std::fprintf(stderr,
                 "usage: %s [--bytes N] [--iters N] [--warmup N] "
                 "[--buffer-size N] [--max-line-bytes N] "
                 "[--stages s1,s2] [--workloads w1,w2]\nerror: %s\n",
                 argv0, detail);
    return 2;
}

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        // Missing flag values are errors, not silent zeros (a zero --iters
        // would later divide by a zero-width sample set).
        auto next = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::string d = std::string(opt) + " requires a value";
                std::exit(usage_error(argv[0], d.c_str()));
            }
            return argv[++i];
        };
        std::size_t v = 0;
        if (a == "--bytes") {
            if (!parse_size(next("--bytes"), v))
                return usage_error(argv[0], "--bytes: invalid number");
            cfg.bytes = v;
        } else if (a == "--iters") {
            if (!parse_size(next("--iters"), v))
                return usage_error(argv[0], "--iters: invalid number");
            cfg.iters = v;
        } else if (a == "--warmup") {
            if (!parse_size(next("--warmup"), v))
                return usage_error(argv[0], "--warmup: invalid number");
            cfg.warmup = v;
        } else if (a == "--buffer-size") {
            if (!parse_size(next("--buffer-size"), v))
                return usage_error(argv[0], "--buffer-size: invalid number");
            cfg.buffer_size = v;
        } else if (a == "--max-line-bytes") {
            if (!parse_size(next("--max-line-bytes"), v))
                return usage_error(argv[0], "--max-line-bytes: invalid number");
            cfg.max_line_bytes = v;
        } else if (a == "--stages") {
            cfg.stages = split_list(next("--stages"));
        } else if (a == "--workloads") {
            cfg.workloads = split_list(next("--workloads"));
        } else {
            return usage_error(argv[0], "unknown argument");
        }
    }
    // Validate before any measurement: an empty sample set must never reach
    // median_sorted/emit_row (sorted.front() on an empty vector), and a
    // zero-byte workload makes gbps a division by zero.
    if (cfg.bytes < 1)
        return usage_error(argv[0], "--bytes must be >= 1");
    if (cfg.iters < 1)
        return usage_error(argv[0], "--iters must be >= 1");
    if (cfg.buffer_size < 1)
        return usage_error(argv[0], "--buffer-size must be >= 1");
    if (cfg.max_line_bytes < 1)
        return usage_error(argv[0], "--max-line-bytes must be >= 1");

    std::printf(
        "stage,workload,bytes,iters,ns_min,ns_med,ns_max,gbps_med,matches,"
        "ns_samples\n");

    const MemStage mem_stages[] = {
        {"L0_sum", stage_l0_sum},
        {"L0_memchr_nl", stage_l0_memchr_nl},
        {"L1_line_std", stage_l1_line_std},
        {"L1_chunk_anchor", stage_l1_chunk_anchor},
        {"L1_chunk_memmem", stage_l1_chunk_memmem},
        {"L2_matcher", stage_l2_matcher},
        {"L2e_matcher_emit", stage_l2e_matcher_emit},
    };
    const FileStage file_stages[] = {
        {"L3_pread_matcher", stage_l3_pread},
        {"L4_sluice", stage_l4_sluice},
    };

    for (auto& w : grep_wl::matrix(cfg.bytes)) {
        if (!selected(cfg.workloads, w.name)) continue;
        std::string data = grep_wl::generate(w);

        // File-backed stages share one temp file per workload (page-cache
        // hot; cold-cache runs are the runner's concern, not the ladder's).
        // Materialization failure is a run-ending error: silently skipping
        // the file-backed stages would publish an incomplete ladder as if
        // it were complete.
        TempPath tp("sluice_grep_attr");
        {
            int wfd =
                ::open(tp.str().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (wfd < 0) bench_fatal("open workload file", errno);
            std::size_t off = 0;
            while (off < data.size()) {
                ssize_t n = sluice::detail::retry_on_eintr([&] {
                    return ::write(wfd, data.data() + off, data.size() - off);
                });
                if (n < 0) {
                    int e = errno;
                    ::close(wfd);
                    bench_fatal("write workload file", e);
                }
                if (n == 0) {
                    ::close(wfd);
                    bench_fatal("workload write made zero progress", 0);
                }
                off += static_cast<std::size_t>(n);
            }
            if (::close(wfd) != 0)
                bench_fatal("close workload file", errno);
        }

        for (auto& s : mem_stages) {
            if (!selected(cfg.stages, s.name)) continue;
            std::vector<std::uint64_t> ns;
            std::uint64_t v = 0;
            for (std::size_t r = 0; r < cfg.warmup + cfg.iters; ++r) {
                std::uint64_t t0 = now_ns();
                v = s.fn(data, w.pattern, cfg.max_line_bytes);
                std::uint64_t t1 = now_ns();
                if (r >= cfg.warmup) ns.push_back(t1 - t0);
            }
            emit_row(s.name, w.name, data.size(), ns, v);
        }
        for (auto& s : file_stages) {
            if (!selected(cfg.stages, s.name)) continue;
            std::vector<std::uint64_t> ns;
            std::uint64_t matches = 0;
            for (std::size_t r = 0; r < cfg.warmup + cfg.iters; ++r) {
                std::uint64_t t0 = now_ns();
                matches =
                    s.fn(tp.str(), w.pattern, cfg.buffer_size,
                         cfg.max_line_bytes);
                std::uint64_t t1 = now_ns();
                if (r >= cfg.warmup) ns.push_back(t1 - t0);
            }
            emit_row(s.name, w.name, data.size(), ns, matches);
        }
    }
    std::fprintf(stderr, "note: results are environment-sensitive; scoped "
                         "per workload/machine/build.\n");
    return 0;
}
