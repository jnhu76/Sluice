#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <sluice/error.hpp>

namespace bench {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

inline TimePoint now() noexcept { return Clock::now(); }

inline double now_ns() noexcept {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

inline double ns_between(TimePoint a, TimePoint b) noexcept {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

inline double seconds_between(TimePoint a, TimePoint b) noexcept {
    return std::chrono::duration<double>(b - a).count();
}

inline double process_cpu_seconds() noexcept {
    timespec ts{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
        return 0.0;
    }
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

inline long resident_kb() noexcept {
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) {
        return -1;
    }
    long size = 0;
    long resident = 0;
    const int rc = std::fscanf(f, "%ld %ld", &size, &resident);
    std::fclose(f);
    if (rc != 2) {
        return -1;
    }
    return resident * (static_cast<long>(::sysconf(_SC_PAGESIZE)) / 1024);
}

inline long peak_rss_kb() noexcept {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
    return static_cast<long>(usage.ru_maxrss);
}

struct Stats {
    std::size_t n = 0;
    double min_ns = 0;
    double p50_ns = 0;
    double p95_ns = 0;
    double p99_ns = 0;
    double max_ns = 0;
    double mean_ns = 0;
    double stddev_ns = 0;
};

inline double quantile_sorted(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double raw = q * static_cast<double>(sorted.size() - 1);
    const auto index = static_cast<std::size_t>(std::round(raw));
    return sorted[std::min(index, sorted.size() - 1)];
}

inline Stats compute_stats(std::vector<double> samples) {
    Stats s;
    if (samples.empty()) {
        return s;
    }
    std::sort(samples.begin(), samples.end());
    s.n = samples.size();
    s.min_ns = samples.front();
    s.p50_ns = quantile_sorted(samples, 0.50);
    s.p95_ns = quantile_sorted(samples, 0.95);
    s.p99_ns = quantile_sorted(samples, 0.99);
    s.max_ns = samples.back();
    double sum = 0.0;
    for (double v : samples) {
        sum += v;
    }
    s.mean_ns = sum / static_cast<double>(s.n);
    double var = 0.0;
    for (double v : samples) {
        const double d = v - s.mean_ns;
        var += d * d;
    }
    s.stddev_ns = std::sqrt(var / static_cast<double>(s.n));
    return s;
}

constexpr std::size_t kPatternPeriod = 65536;

inline std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// A full mix per byte position: the cheaper high-byte multiply hash is constant
// over the first kPatternPeriod positions, which makes verification vacuous.
inline const std::byte* pattern_table() {
    static const std::vector<std::byte> table = [] {
        std::vector<std::byte> t(kPatternPeriod);
        for (std::size_t i = 0; i < kPatternPeriod; ++i) {
            t[i] = static_cast<std::byte>(
                static_cast<std::uint8_t>(mix64(static_cast<std::uint64_t>(i)) >> 24));
        }
        return t;
    }();
    return table.data();
}

inline std::size_t pattern_distinct_values() {
    const std::byte* t = pattern_table();
    bool seen[256] = {};
    std::size_t distinct = 0;
    for (std::size_t i = 0; i < kPatternPeriod; ++i) {
        const auto v = static_cast<std::size_t>(static_cast<unsigned char>(t[i]));
        if (!seen[v]) {
            seen[v] = true;
            ++distinct;
        }
    }
    return distinct;
}

inline void fill_pattern(std::byte* dst, std::size_t len, std::uint64_t base) noexcept {
    const std::byte* t = pattern_table();
    std::size_t offset = static_cast<std::size_t>(base % kPatternPeriod);
    std::size_t done = 0;
    while (done < len) {
        const std::size_t chunk = std::min(kPatternPeriod - offset, len - done);
        std::memcpy(dst + done, t + offset, chunk);
        done += chunk;
        offset = 0;
    }
}

struct MismatchReport {
    bool ok = true;
    std::size_t bad_segments = 0;
    std::uint64_t first_bad_position = 0;
    std::byte found{};
    std::byte expected{};
};

inline MismatchReport verify_pattern(const std::byte* src, std::size_t len,
                                     std::uint64_t base) noexcept {
    MismatchReport r;
    const std::byte* t = pattern_table();
    std::size_t offset = static_cast<std::size_t>(base % kPatternPeriod);
    std::size_t done = 0;
    while (done < len) {
        const std::size_t chunk = std::min(kPatternPeriod - offset, len - done);
        if (std::memcmp(src + done, t + offset, chunk) != 0) {
            if (r.ok) {
                r.ok = false;
                for (std::size_t i = 0; i < chunk; ++i) {
                    if (src[done + i] != t[offset + i]) {
                        r.first_bad_position = base + done + i;
                        r.found = src[done + i];
                        r.expected = t[offset + i];
                        break;
                    }
                }
            }
            ++r.bad_segments;
        }
        done += chunk;
        offset = 0;
    }
    return r;
}

// Gate for the oracle itself, run before any case: a degenerate pattern table,
// an offset confusion or a non-biting comparison must fail here.
struct OracleSelfTest {
    bool ok = true;
    std::string failure;
};

inline OracleSelfTest oracle_self_test() {
    OracleSelfTest result;
    const std::size_t distinct = pattern_distinct_values();
    if (distinct < 200) {
        result.ok = false;
        result.failure = "pattern table has only " + std::to_string(distinct) +
                         " distinct byte values";
        return result;
    }
    std::vector<std::byte> buffer(kPatternPeriod + 4096);
    constexpr std::uint64_t kBase = 123456;
    fill_pattern(buffer.data(), 4096, kBase);
    if (!verify_pattern(buffer.data(), 4096, kBase).ok) {
        result.ok = false;
        result.failure = "a correctly filled buffer failed verification";
        return result;
    }
    if (verify_pattern(buffer.data(), 4096, kBase + 1).ok) {
        result.ok = false;
        result.failure = "an off-by-one offset was not detected";
        return result;
    }
    buffer[2000] ^= std::byte{0x01};
    const MismatchReport flipped = verify_pattern(buffer.data(), 4096, kBase);
    if (flipped.ok || flipped.first_bad_position != kBase + 2000) {
        result.ok = false;
        result.failure = "a single flipped byte was not reported at the right position";
        return result;
    }
    buffer[2000] ^= std::byte{0x01};
    std::vector<std::byte> zeros(4096, std::byte{0});
    if (verify_pattern(zeros.data(), 4096, kBase).ok) {
        result.ok = false;
        result.failure = "an all-zero buffer passed verification (degenerate pattern)";
        return result;
    }
    return result;
}

struct Record {
    std::string case_id;
    std::string workload;
    std::string backend;
    std::string dataset;
    std::string cache_policy;
    std::string filesystem;
    std::string notes;

    std::size_t block_size = 0;
    std::size_t request_count = 0;
    std::size_t concurrency = 0;
    std::size_t queue_depth = 0;
    std::size_t worker_count = 0;
    std::size_t warmup = 0;
    std::size_t rep = 0;
    std::size_t order_index = 0;
    bool measured = true;

    bool ok = true;
    std::string failure;

    double wall_s = 0.0;
    double ops = 0.0;
    double bytes = 0.0;
    double ops_per_sec = 0.0;
    double mib_per_sec = 0.0;

    double cpu_s = 0.0;
    double cpu_util = 0.0;
    long rss_kb = 0;
    long peak_rss_kb = 0;

    Stats latency;

    std::uint64_t submits = 0;
    std::uint64_t would_block = 0;
    std::uint64_t notifications = 0;
    std::uint64_t empty_wakes = 0;
    std::uint64_t passes = 0;
    std::uint64_t waits = 0;
    std::uint64_t extra_a = 0;
    std::uint64_t extra_b = 0;

    std::uint64_t stats_submit_calls = 0;
    std::uint64_t stats_submitted_ops = 0;
    std::uint64_t stats_poll_calls = 0;
    std::uint64_t stats_wait_calls = 0;
    std::uint64_t stats_completed_ops = 0;
    std::uint64_t stats_canceled_ops = 0;
    std::uint64_t stats_completion_errors = 0;
    std::uint64_t stats_short_completions = 0;
    std::uint64_t stats_max_outstanding = 0;
    std::uint64_t stats_queue_full_retries = 0;
    std::uint64_t stats_invalid_state = 0;
};

inline std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

inline std::FILE* jsonl_stream() {
    static std::FILE* stream = [] {
        const char* path = std::getenv("SLUICE_BENCH_JSONL");
        if (path == nullptr || *path == '\0') {
            return static_cast<std::FILE*>(nullptr);
        }
        return std::fopen(path, "a");
    }();
    return stream;
}

inline void emit_record(const Record& r) {
    std::FILE* out = jsonl_stream();
    if (out == nullptr) {
        return;
    }
    std::fprintf(
        out,
        "{\"case_id\":\"%s\",\"workload\":\"%s\",\"backend\":\"%s\",\"dataset\":\"%s\","
        "\"cache_policy\":\"%s\",\"filesystem\":\"%s\",\"notes\":\"%s\","
        "\"block_size\":%zu,\"request_count\":%zu,\"concurrency\":%zu,\"queue_depth\":%zu,"
        "\"worker_count\":%zu,\"warmup\":%zu,\"rep\":%zu,\"order_index\":%zu,\"measured\":%s,"
        "\"ok\":%s,\"failure\":\"%s\","
        "\"wall_s\":%.9f,\"ops\":%.0f,\"bytes\":%.0f,\"ops_per_sec\":%.3f,\"mib_per_sec\":%.3f,"
        "\"cpu_s\":%.9f,\"cpu_util\":%.6f,\"rss_kb\":%ld,\"peak_rss_kb\":%ld,"
        "\"lat_n\":%zu,\"lat_min_ns\":%.1f,\"lat_p50_ns\":%.1f,\"lat_p95_ns\":%.1f,"
        "\"lat_p99_ns\":%.1f,\"lat_max_ns\":%.1f,\"lat_mean_ns\":%.1f,\"lat_stddev_ns\":%.1f,"
        "\"submits\":%llu,\"would_block\":%llu,\"notifications\":%llu,\"empty_wakes\":%llu,"
        "\"passes\":%llu,\"waits\":%llu,\"extra_a\":%llu,\"extra_b\":%llu,"
        "\"stats_submit_calls\":%llu,\"stats_submitted_ops\":%llu,\"stats_poll_calls\":%llu,"
        "\"stats_wait_calls\":%llu,\"stats_completed_ops\":%llu,\"stats_canceled_ops\":%llu,"
        "\"stats_completion_errors\":%llu,\"stats_short_completions\":%llu,"
        "\"stats_max_outstanding\":%llu,\"stats_queue_full_retries\":%llu,"
        "\"stats_invalid_state\":%llu}\n",
        json_escape(r.case_id).c_str(), json_escape(r.workload).c_str(),
        json_escape(r.backend).c_str(), json_escape(r.dataset).c_str(),
        json_escape(r.cache_policy).c_str(), json_escape(r.filesystem).c_str(),
        json_escape(r.notes).c_str(), r.block_size, r.request_count, r.concurrency,
        r.queue_depth, r.worker_count, r.warmup, r.rep, r.order_index,
        r.measured ? "true" : "false", r.ok ? "true" : "false", json_escape(r.failure).c_str(),
        r.wall_s, r.ops, r.bytes, r.ops_per_sec, r.mib_per_sec, r.cpu_s, r.cpu_util, r.rss_kb,
        r.peak_rss_kb, r.latency.n, r.latency.min_ns, r.latency.p50_ns, r.latency.p95_ns,
        r.latency.p99_ns, r.latency.max_ns, r.latency.mean_ns, r.latency.stddev_ns,
        static_cast<unsigned long long>(r.submits),
        static_cast<unsigned long long>(r.would_block),
        static_cast<unsigned long long>(r.notifications),
        static_cast<unsigned long long>(r.empty_wakes),
        static_cast<unsigned long long>(r.passes), static_cast<unsigned long long>(r.waits),
        static_cast<unsigned long long>(r.extra_a), static_cast<unsigned long long>(r.extra_b),
        static_cast<unsigned long long>(r.stats_submit_calls),
        static_cast<unsigned long long>(r.stats_submitted_ops),
        static_cast<unsigned long long>(r.stats_poll_calls),
        static_cast<unsigned long long>(r.stats_wait_calls),
        static_cast<unsigned long long>(r.stats_completed_ops),
        static_cast<unsigned long long>(r.stats_canceled_ops),
        static_cast<unsigned long long>(r.stats_completion_errors),
        static_cast<unsigned long long>(r.stats_short_completions),
        static_cast<unsigned long long>(r.stats_max_outstanding),
        static_cast<unsigned long long>(r.stats_queue_full_retries),
        static_cast<unsigned long long>(r.stats_invalid_state));
    std::fflush(out);
}

inline void print_record(const Record& r) {
    if (!r.ok) {
        std::printf("  [%s rep=%zu] FAILED: %s\n", r.case_id.c_str(), r.rep, r.failure.c_str());
        return;
    }
    std::printf(
        "  [%s rep=%zu%s] ops=%.0f wall=%.4fs %.0f ops/s %.1f MiB/s cpu=%.3fs util=%.1f%% | "
        "lat p50=%.0f p95=%.0f p99=%.0f max=%.0f ns (n=%zu)\n",
        r.case_id.c_str(), r.rep, r.measured ? "" : " warmup", r.ops, r.wall_s, r.ops_per_sec,
        r.mib_per_sec, r.cpu_s, r.cpu_util * 100.0, r.latency.p50_ns, r.latency.p95_ns,
        r.latency.p99_ns, r.latency.max_ns, r.latency.n);
}

// One timestamp pair; the throughput phase must not call the clock inside its
// timed region, so this cost is reported separately.
inline double clock_overhead_ns(std::size_t iterations = 200000) {
    volatile std::uint64_t sink = 0;
    const TimePoint start = now();
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto a = Clock::now().time_since_epoch().count();
        const auto b = Clock::now().time_since_epoch().count();
        sink = static_cast<std::uint64_t>(a ^ b);
    }
    const TimePoint end = now();
    (void)sink;
    return ns_between(start, end) / static_cast<double>(iterations * 2);
}

struct EnvConfig {
    std::size_t warmup = 2;
    std::size_t measured = 7;
    std::string tmp_dir = "/tmp/sluice-bench";
    std::string disk_dir;
    bool include_disk = true;
};

inline EnvConfig env_config() {
    EnvConfig cfg;
    if (const char* v = std::getenv("SLUICE_BENCH_WARMUP")) {
        cfg.warmup = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    }
    if (const char* v = std::getenv("SLUICE_BENCH_MEASURED")) {
        cfg.measured = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    }
    if (const char* v = std::getenv("SLUICE_BENCH_TMP_DIR")) {
        cfg.tmp_dir = v;
    }
    if (const char* v = std::getenv("SLUICE_BENCH_DISK_DIR")) {
        cfg.disk_dir = v;
    } else {
        const char* home = std::getenv("HOME");
        cfg.disk_dir = home != nullptr ? std::string(home) + "/.cache/sluice-bench" : "";
    }
    if (const char* v = std::getenv("SLUICE_BENCH_SKIP_DISK")) {
        cfg.include_disk = v[0] == '0';
    }
    cfg.include_disk = cfg.include_disk && !cfg.disk_dir.empty();
    return cfg;
}

inline void ensure_dir(const std::string& path) {
    if (path.empty()) {
        return;
    }
    const std::string cmd = "mkdir -p '" + path + "'";
    const int rc = std::system(cmd.c_str());
    (void)rc;
}

// Per-round rotation: no case is measured first in every round, so drift over
// the run is spread across the comparable cases.
inline std::vector<std::size_t> rotation_order(std::size_t count, std::size_t round) {
    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        order[i] = (i + round) % count;
    }
    return order;
}

inline bool case_selected(const std::string& case_id) {
    const char* filter = std::getenv("SLUICE_BENCH_CASE_FILTER");
    if (filter == nullptr || *filter == '\0') {
        return true;
    }
    if (filter[0] == '=') {
        return case_id == (filter + 1);
    }
    return case_id.find(filter) != std::string::npos;
}

struct RunOutcome {
    bool ok = true;
    std::string failure;
    double wall_s = 0.0;
    std::size_t ops = 0;
    std::size_t bytes = 0;
};

// Uninstrumented timed region; the operation reports its transferred byte count
// or fails with a message.
template <class Op>
inline RunOutcome time_ops(std::size_t count, Op&& op) {
    RunOutcome r;
    std::size_t total_bytes = 0;
    const TimePoint start = now();
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t op_bytes = 0;
        if (!op(i, op_bytes, r.failure)) {
            r.ok = false;
            return r;
        }
        total_bytes += op_bytes;
    }
    const TimePoint end = now();
    r.wall_s = seconds_between(start, end);
    r.ops = count;
    r.bytes = total_bytes;
    return r;
}

// Instrumented companion pass: the same operations with per-operation clock
// pairs, never folded into the throughput numbers.
template <class Op>
inline bool time_ops_perop(std::size_t count, Op&& op, std::vector<double>& samples,
                           std::string& failure, std::size_t max_samples) {
    samples.clear();
    const std::size_t stride = count > max_samples ? (count / max_samples) : 1;
    samples.reserve(std::min(count, max_samples));
    for (std::size_t i = 0; i < count; i += stride) {
        std::size_t op_bytes = 0;
        const TimePoint a = now();
        const bool ok = op(i, op_bytes, failure);
        const TimePoint b = now();
        if (!ok) {
            return false;
        }
        samples.push_back(ns_between(a, b));
        if (samples.size() >= max_samples) {
            break;
        }
    }
    return true;
}

struct CaseMeta {
    std::string case_id;
    std::string workload;
    std::string backend;
    std::string dataset;
    std::string cache_policy;
    std::string filesystem;
    std::string notes;
    std::size_t block_size = 0;
    std::size_t request_count = 0;
    std::size_t concurrency = 0;
    std::size_t queue_depth = 0;
    std::size_t worker_count = 0;
};

inline Record make_record(const CaseMeta& meta, const EnvConfig& cfg, std::size_t rep,
                          bool measured, std::size_t order_index) {
    Record r;
    r.case_id = meta.case_id;
    r.workload = meta.workload;
    r.backend = meta.backend;
    r.dataset = meta.dataset;
    r.cache_policy = meta.cache_policy;
    r.filesystem = meta.filesystem;
    r.notes = meta.notes;
    r.block_size = meta.block_size;
    r.request_count = meta.request_count;
    r.concurrency = meta.concurrency;
    r.queue_depth = meta.queue_depth;
    r.worker_count = meta.worker_count;
    r.warmup = cfg.warmup;
    r.rep = rep;
    r.order_index = order_index;
    r.measured = measured;
    return r;
}

// Records the gate outcome in the raw evidence and exits non-zero if it failed.
inline int oracle_self_test_case(const EnvConfig& cfg) {
    CaseMeta meta;
    meta.case_id = "CAL-oracle-self-test";
    meta.workload = "calibration";
    meta.backend = "n/a";
    meta.dataset = "pattern table + verify_pattern discrimination";
    meta.cache_policy = "n/a";
    meta.filesystem = "n/a";
    meta.notes = "must pass before any case; a degenerate pattern or a non-biting "
                 "comparison fails here";
    Record r = make_record(meta, cfg, 0, false, 0);
    const OracleSelfTest result = oracle_self_test();
    r.ok = result.ok;
    r.failure = result.failure;
    emit_record(r);
    if (!result.ok) {
        std::printf("  [%s] FAILED: %s\n", meta.case_id.c_str(), result.failure.c_str());
    } else {
        std::printf("  [%s] pattern distinct values=%zu; offset confusion, single-byte "
                    "flip and all-zero buffers are all detected\n",
                    meta.case_id.c_str(), pattern_distinct_values());
    }
    return result.ok ? 0 : 1;
}

// Warmup plus measured repetitions; a failing repetition is emitted as failing
// and is never dropped from the raw record.
template <class PrepareFn, class ThroughputFn, class VerifyFn, class LatencyFn>
inline int run_case_reps(const CaseMeta& meta, const EnvConfig& cfg, std::size_t order_index,
                         PrepareFn&& prepare, ThroughputFn&& throughput, VerifyFn&& verify,
                         LatencyFn&& latency, std::vector<std::byte>& scratch) {
    int failed = 0;
    for (std::size_t rep = 0; rep < cfg.warmup + cfg.measured; ++rep) {
        const bool measured = rep >= cfg.warmup;
        Record r =
            make_record(meta, cfg, measured ? rep - cfg.warmup : rep, measured, order_index);
        std::string failure;
        if (!prepare(rep, failure)) {
            r.ok = false;
            r.failure = "prepare: " + failure;
            emit_record(r);
            print_record(r);
            failed = 1;
            continue;
        }
        const double cpu_before = process_cpu_seconds();
        const RunOutcome run = throughput(scratch, failure);
        const double cpu_after = process_cpu_seconds();
        if (!run.ok) {
            r.ok = false;
            r.failure = "throughput: " + failure;
            emit_record(r);
            print_record(r);
            failed = 1;
            continue;
        }
        r.wall_s = run.wall_s;
        r.ops = static_cast<double>(run.ops);
        r.bytes = static_cast<double>(run.bytes);
        r.ops_per_sec = run.wall_s > 0.0 ? r.ops / run.wall_s : 0.0;
        r.mib_per_sec = run.wall_s > 0.0 ? (r.bytes / (1024.0 * 1024.0)) / run.wall_s : 0.0;
        r.cpu_s = cpu_after - cpu_before;
        r.cpu_util = run.wall_s > 0.0 ? r.cpu_s / run.wall_s : 0.0;
        r.rss_kb = resident_kb();
        r.peak_rss_kb = peak_rss_kb();
        if (!verify(scratch, failure)) {
            r.ok = false;
            r.failure = "correctness oracle: " + failure;
            failed = 1;
        }
        std::vector<double> samples;
        if (!latency(samples, failure)) {
            r.ok = false;
            r.failure = "latency pass: " + failure;
            failed = 1;
        } else {
            r.latency = compute_stats(samples);
        }
        emit_record(r);
        print_record(r);
    }
    return failed;
}

} // namespace bench
