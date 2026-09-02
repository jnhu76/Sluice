// TAX-0 EXP-U0 router-scan causal-ablation bench (#250 TAX-0 campaign).
//
// EXPERIMENT QUESTION (RQ-U0, preregistered in the EXP-U0 task): does the
// per-operation CQE lookup through find_live_router_cookie_ CAUSE the
// material C-dependent Uring instruction tax measured by EXP-0? This
// binary drives the EXACT EXP-0 workload shape (depth-D submit/await READ
// pipeline over ApplicationRuntime + REAL UringAsyncBackend) under a
// selectable, research-only router scan direction:
//
//   --router-scan-mode reverse_production  (the shipped production scan —
//                                         reverse since the R1 landing;
//                                         baseline)
//   --router-scan-mode forward_ablation    (same predicate, low->high — the
//                                         pre-fix traversal, the EXP-U0
//                                         causal-comparator direction)
//
// The scan mode is set through the SLUICE_ASYNC_INTERNAL_TESTING-only seam
// (set_router_scan_mode_for_test) BEFORE the runtime starts driving the
// backend; both arms execute the identical semantic matching predicate and
// the identical fail-closed same-work accounting (ops/bytes/word sum), so
// any performance difference is attributable to scan direction alone.
//
// Per-repetition router diagnostics (the U0-A exact scan witness) are
// snapshotted from the seam counters at rep boundaries (backend quiescent
// between reps: the task completed, outstanding==0, and the single-driver
// domain owns the counters), so every measured row carries its own
// scan-iteration accounting.
//
// This target deliberately links sluice_async_internal_testing (NOT the
// production sluice_async): the whole binary IS the research instrument.
// Production Release builds of sluice_async compile none of the seam and
// keep the production reverse scan (R1) byte-identical.
//
// Output: one JSON object on stdout (consumed by scripts/bench/
// perf-attribution.py `tax0u0`). Exit 0 only when every counted
// repetition completed with exact op/byte accounting.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/detail/posix_retry.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

namespace {

// ---------------------------------------------------------------------------
// Fail-closed helpers
// ---------------------------------------------------------------------------

[[noreturn]] void bench_fatal(const char* what, int err) {
    std::fprintf(stderr, "tax0u0_router_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(3);
}

[[noreturn]] void bench_semantic(const char* what) {
    std::fprintf(stderr, "tax0u0_router_bench: semantic failure: %s\n", what);
    std::exit(3);
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct CpuTime {
    std::uint64_t user_ns = 0;
    std::uint64_t sys_ns = 0;
    std::uint64_t maxrss_kb = 0;
};

CpuTime cpu_time_now() {
    struct rusage ru;
    if (::getrusage(RUSAGE_SELF, &ru) != 0)
        bench_fatal("getrusage", errno);
    CpuTime t;
    t.user_ns = static_cast<std::uint64_t>(ru.ru_utime.tv_sec) * 1000000000ull +
                static_cast<std::uint64_t>(ru.ru_utime.tv_usec) * 1000ull;
    t.sys_ns = static_cast<std::uint64_t>(ru.ru_stime.tv_sec) * 1000000000ull +
               static_cast<std::uint64_t>(ru.ru_stime.tv_usec) * 1000ull;
    t.maxrss_kb = static_cast<std::uint64_t>(ru.ru_maxrss);  // KiB on Linux
    return t;
}

// ---------------------------------------------------------------------------
// Deterministic workload bytes — IDENTICAL generator to
// tax0_capacity_bench (same kSeed, same splitmix64 master block), so the
// U0 arms and the EXP-0 cells share byte-identical input.
// ---------------------------------------------------------------------------

constexpr std::size_t kBlock = 4096;
constexpr std::uint64_t kSeed = 0xE1E1E1E121212121ull;

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

void fill_master_block(std::uint64_t* words) {
    for (std::size_t i = 0; i < kBlock / sizeof(std::uint64_t); ++i)
        words[i] = splitmix64(kSeed + i);
}

std::uint64_t block_word_sum(const std::uint64_t* words) {
    std::uint64_t s = 0;
    for (std::size_t i = 0; i < kBlock / sizeof(std::uint64_t); ++i)
        s += words[i];
    return s;
}

void fill_from_master(std::byte* dst, std::size_t len, const std::byte* master) {
    for (std::size_t off = 0; off < len; off += kBlock)
        std::memcpy(dst + off, master, kBlock);
}

std::uint64_t word_sum(const std::byte* p, std::size_t len) {
    auto* w = reinterpret_cast<const std::uint64_t*>(p);
    std::uint64_t s = 0;
    for (std::size_t i = 0; i < len / sizeof(std::uint64_t); ++i)
        s += w[i];
    return s;
}

// ---------------------------------------------------------------------------
// Configuration + validation
// ---------------------------------------------------------------------------

enum class ScanMode { forward_ablation, reverse_production };

struct Config {
    ScanMode scan_mode = ScanMode::reverse_production;
    std::size_t request_size = 4096;
    std::size_t total_bytes = 256u << 20;
    std::size_t depth = 8;
    std::size_t request_capacity = 8;
    unsigned uring_queue_depth = 8;
    std::size_t reps = 11;
    std::size_t warmup = 0;
    std::string file;
};

bool config_valid(const Config& c, std::string& err) {
    if (c.request_size == 0 || c.request_size % kBlock != 0) {
        err = "--request-size must be a positive multiple of 4096";
        return false;
    }
    if (c.total_bytes == 0 || c.total_bytes % c.request_size != 0) {
        err = "--total-bytes must be a positive multiple of --request-size";
        return false;
    }
    if (c.depth == 0 || c.depth > 1024) {
        err = "--depth must be in [1, 1024]";
        return false;
    }
    if (c.request_capacity < c.depth || c.request_capacity > 65536) {
        err = "--request-capacity must be in [depth, 65536]";
        return false;
    }
    if (c.uring_queue_depth == 0 || c.uring_queue_depth > 32768) {
        err = "--uring-queue-depth must be in [1, 32768]";
        return false;
    }
    if (c.reps == 0 || c.reps > 100000 || c.warmup > 1000) {
        err = "--reps must be >= 1 and --warmup bounded";
        return false;
    }
    constexpr std::size_t kMaxBuf = 1ull << 30;  // 1 GiB buffer budget
    if (c.depth * c.request_size > kMaxBuf) {
        err = "depth * request-size exceeds the 1 GiB buffer budget";
        return false;
    }
    if (c.file.empty()) {
        err = "--file is required";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shared run state
// ---------------------------------------------------------------------------

struct RepResult {
    std::uint64_t wall_ns = 0;
    std::uint64_t user_ns = 0;
    std::uint64_t sys_ns = 0;
    std::uint64_t maxrss_kb = 0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    std::uint64_t word_sum = 0;
    std::uint64_t errors = 0;
    // U0-A scan witness snapshot for exactly this rep.
    std::uint64_t op_lookup_calls = 0;
    std::uint64_t op_lookup_iterations_total = 0;
    std::uint64_t op_lookup_iterations_max = 0;
    std::uint64_t control_lookup_calls = 0;
    std::uint64_t transport_lookup_calls = 0;
    std::uint64_t lookup_hits = 0;
    std::uint64_t lookup_misses = 0;
    std::uint64_t matched_router_index_sum = 0;
    std::uint64_t matched_router_index_max = 0;
};

struct RunState {
    Config cfg;
    std::vector<std::uint64_t> master_words;  // kBlock/8 splitmix64 words
    std::uint64_t expected_sum = 0;
    std::size_t ops = 0;
    int fd = -1;
    std::vector<std::vector<std::byte>> buf;  // depth slots x request_size
    // Pipeline Completions: process-lifetime, address-stable (L7),
    // default-constructed in place (non-movable by contract).
    std::vector<sluice::async::Completion<std::size_t>> comp;

    const std::byte* master_bytes() const {
        return reinterpret_cast<const std::byte*>(master_words.data());
    }

    void init_buffers() {
        buf.resize(cfg.depth);
        for (auto& b : buf) b.assign(cfg.request_size, std::byte{0});
        comp = std::vector<sluice::async::Completion<std::size_t>>(cfg.depth);
    }
};

// ---------------------------------------------------------------------------
// File preparation (untimed) — identical semantics to tax0_capacity_bench.
// READ-only experiment: prepare the deterministic input file if absent.
// ---------------------------------------------------------------------------

void prepare_file(const Config& c, const RunState& rs) {
    int probe = ::open(c.file.c_str(), O_RDONLY);
    if (probe >= 0) {
        off_t sz = ::lseek(probe, 0, SEEK_END);
        int e = errno;
        ::close(probe);
        if (sz < 0) bench_fatal("lseek input file", e);
        if (static_cast<std::uint64_t>(sz) == c.total_bytes) return;
    }
    int fd = ::open(c.file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) bench_fatal("open input file", errno);
    constexpr std::size_t kChunk = 1u << 20;
    std::vector<std::byte> chunk(kChunk);
    fill_from_master(chunk.data(), kChunk, rs.master_bytes());
    std::size_t off = 0;
    while (off < c.total_bytes) {
        std::size_t n = std::min(kChunk, c.total_bytes - off);
        ssize_t w = sluice::detail::retry_on_eintr(
            [&] { return ::write(fd, chunk.data(), n); });
        if (w < 0) {
            int e = errno;
            ::close(fd);
            bench_fatal("write input file", e);
        }
        if (w == 0) {
            ::close(fd);
            bench_semantic("input-file write made zero progress");
        }
        off += static_cast<std::size_t>(w);
    }
    if (::close(fd) != 0) bench_fatal("close input file", errno);
}

// ---------------------------------------------------------------------------
// The measured path: one task drives a depth-D submit/await READ pipeline
// (identical to tax0_capacity_bench's read arm).
// ---------------------------------------------------------------------------

struct TaskOutcome {
    std::uint64_t word_sum = 0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    bool ok = false;
    char err[128] = {0};
};

void task_body(sluice::async::RuntimeTaskContext& ctx, RunState& rs,
               TaskOutcome& out) {
    using namespace sluice::async;
    const Config& c = rs.cfg;
    std::size_t submit_k = 0;
    std::size_t await_k = 0;
    while (await_k < rs.ops) {
        while (submit_k < rs.ops && submit_k - await_k < c.depth) {
            std::byte* b = rs.buf[submit_k % c.depth].data();
            std::uint64_t off =
                static_cast<std::uint64_t>(submit_k) * c.request_size;
            Completion<std::size_t>& cc = rs.comp[submit_k % c.depth];
            auto sr = ctx.submit_read(ReadOp{rs.fd, b, c.request_size, off}, cc);
            if (!sr.has_value()) {
                std::snprintf(out.err, sizeof(out.err),
                              "submit_read rejected (code %d)",
                              static_cast<int>(sr.error().code));
                return;
            }
            ++submit_k;
        }
        Completion<std::size_t>& cc = rs.comp[await_k % c.depth];
        auto wr = ctx.await_completion(cc);
        if (!wr.has_value()) {
            std::snprintf(out.err, sizeof(out.err),
                          "await_completion rejected (code %d)",
                          static_cast<int>(wr.error().code));
            return;
        }
        auto res = cc.result();
        if (!res.has_value()) {
            std::snprintf(out.err, sizeof(out.err),
                          "terminal I/O error (code %d, os %d)",
                          static_cast<int>(res.error().code),
                          res.error().os_errno);
            return;
        }
        std::size_t n = res.value();
        if (n != c.request_size) {
            std::snprintf(out.err, sizeof(out.err),
                          "op %zu moved %zu bytes (want %zu)", await_k, n,
                          c.request_size);
            return;
        }
        out.word_sum +=
            word_sum(rs.buf[await_k % c.depth].data(), c.request_size);
        cc.reset();
        ++await_k;
        ++out.ops;
        out.bytes += n;
    }
    out.ok = true;
}

RepResult run_one_rep(sluice::async::ApplicationRuntime& rt, RunState& rs,
                      sluice::async::UringAsyncBackend* backend) {
    using namespace sluice::async;
    RepResult r;
    // Reset the scan witness BEFORE the rep; the backend is quiescent here
    // (previous rep's task completed; single-driver domain owns counters).
    backend->reset_router_scan_diagnostics_for_test();
    CpuTime c0 = cpu_time_now();
    std::uint64_t t0 = now_ns();
    TaskResultSlot<TaskOutcome> slot;
    auto sub = rt.submit([&rs, &slot](RuntimeTaskContext& ctx) {
        TaskOutcome out;
        task_body(ctx, rs, out);
        slot.publish(std::move(out));
    });
    if (!sub.has_value())
        bench_semantic("runtime rejected the task admission");
    TaskOutcome out = slot.wait_and_take();
    std::uint64_t t1 = now_ns();
    CpuTime c1 = cpu_time_now();
    r.wall_ns = t1 - t0;
    r.user_ns = c1.user_ns - c0.user_ns;
    r.sys_ns = c1.sys_ns - c0.sys_ns;
    r.maxrss_kb = c1.maxrss_kb;
    r.word_sum = out.word_sum;
    r.ops = out.ops;
    r.bytes = out.bytes;
    // Snapshot the scan witness AFTER the task completed (all CQEs reaped:
    // outstanding==0; the last await consumed every op CQE).
    const auto& diag = backend->router_scan_diagnostics_for_test();
    r.op_lookup_calls = diag.operation_cookie_lookup_calls;
    r.op_lookup_iterations_total = diag.operation_lookup_iterations_total;
    r.op_lookup_iterations_max = diag.operation_lookup_iterations_max;
    r.control_lookup_calls = diag.control_cookie_lookup_calls;
    r.transport_lookup_calls = diag.transport_cookie_lookup_calls;
    r.lookup_hits = diag.lookup_hits;
    r.lookup_misses = diag.lookup_misses;
    r.matched_router_index_sum = diag.matched_router_index_sum;
    r.matched_router_index_max = diag.matched_router_index_max;
    if (!out.ok) {
        std::fprintf(stderr, "tax0u0_router_bench: task failed: %s\n",
                     out.err);
        r.errors = 1;
    }
    return r;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

bool parse_size(const char* s, std::size_t& out) {
    if (s == nullptr || *s == '\0') return false;
    std::size_t v = 0;
    const std::size_t limit = SIZE_MAX;
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
    std::fprintf(
        stderr,
        "usage: %s --file PATH [--router-scan-mode forward|reverse]\n"
        "          [--request-size N] [--total-bytes N] [--depth N]\n"
        "          [--request-capacity N] [--uring-queue-depth N]\n"
        "          [--reps N] [--warmup N]\n"
        "note: EXP-U0 research bench. uring READ only; scan mode selects\n"
        "      reverse_production (baseline) or forward_ablation. Requires\n"
        "      a real-liburing build (xmake f --with-liburing=true).\n"
        "error: %s\n",
        argv0, detail);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    using namespace sluice::async;

    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::string d = std::string(opt) + " requires a value";
                std::exit(usage_error(argv[0], d.c_str()));
            }
            return argv[++i];
        };
        auto size_opt = [&](const char* opt, std::size_t& out) {
            if (!parse_size(next(opt), out)) {
                std::string d = std::string(opt) + ": invalid whole number";
                std::exit(usage_error(argv[0], d.c_str()));
            }
        };
        if (a == "--router-scan-mode") {
            std::string s(next("--router-scan-mode"));
            if (s == "forward") cfg.scan_mode = ScanMode::forward_ablation;
            else if (s == "reverse") cfg.scan_mode = ScanMode::reverse_production;
            else return usage_error(
                argv[0], "--router-scan-mode must be forward|reverse");
        } else if (a == "--request-size") {
            size_opt("--request-size", cfg.request_size);
        } else if (a == "--total-bytes") {
            size_opt("--total-bytes", cfg.total_bytes);
        } else if (a == "--depth") {
            size_opt("--depth", cfg.depth);
        } else if (a == "--request-capacity") {
            size_opt("--request-capacity", cfg.request_capacity);
        } else if (a == "--uring-queue-depth") {
            std::size_t q = 0;
            size_opt("--uring-queue-depth", q);
            cfg.uring_queue_depth = static_cast<unsigned>(q);
        } else if (a == "--reps") {
            size_opt("--reps", cfg.reps);
        } else if (a == "--warmup") {
            size_opt("--warmup", cfg.warmup);
        } else if (a == "--file") {
            cfg.file = next("--file");
        } else {
            return usage_error(argv[0], "unknown argument");
        }
    }
    std::string cfg_err;
    if (!config_valid(cfg, cfg_err))
        return usage_error(argv[0], cfg_err.c_str());

    RunState rs;
    rs.cfg = cfg;
    rs.master_words.assign(kBlock / sizeof(std::uint64_t), 0);
    fill_master_block(rs.master_words.data());
    rs.expected_sum =
        block_word_sum(rs.master_words.data()) * (cfg.total_bytes / kBlock);
    rs.ops = cfg.total_bytes / cfg.request_size;

    prepare_file(cfg, rs);

    rs.fd = ::open(cfg.file.c_str(), O_RDONLY);
    if (rs.fd < 0) bench_fatal("open data file", errno);
    rs.init_buffers();

    // Runtime lifecycle is measured OUTSIDE the per-rep windows: build +
    // start before the first rep, request_stop/drain/join after the last.
    std::uint64_t setup_ns = 0, teardown_ns = 0;
    std::uint64_t s0 = now_ns();
    std::unique_ptr<ApplicationRuntime> rt;
    UringAsyncBackend* backend = nullptr;
    {
        RuntimeBuilder builder;
        builder.workers(1);  // E1-L2 shape: one task drives the pipeline
        auto ub = std::make_unique<UringAsyncBackend>(UringConfig{
            cfg.request_capacity, cfg.uring_queue_depth});
        if (!ub->available()) {
            std::fprintf(stderr,
                         "tax0u0_router_bench: uring backend did not "
                         "initialize a real ring (available()==false)\n");
            return 3;
        }
        // Install the research scan mode BEFORE the runtime starts driving
        // the backend (quiescent, single thread).
        ub->set_router_scan_mode_for_test(
            cfg.scan_mode == ScanMode::reverse_production
                ? UringAsyncBackend::RouterScanModeForTest::reverse_production
                : UringAsyncBackend::RouterScanModeForTest::forward_ablation);
        backend = ub.get();
        builder.backend(std::move(ub));
        auto built = builder.build();
        if (!built.has_value())
            bench_semantic("RuntimeBuilder::build rejected the config");
        rt = std::move(built.value());
        auto started = rt->start();
        if (!started.has_value())
            bench_semantic("ApplicationRuntime::start failed");
    }
    setup_ns = now_ns() - s0;

    std::vector<RepResult> results;
    for (std::size_t r = 0; r < cfg.warmup + cfg.reps; ++r) {
        RepResult res = run_one_rep(*rt, rs, backend);
        if (r >= cfg.warmup) results.push_back(res);
    }

    std::uint64_t s1 = now_ns();
    rt->request_stop();
    auto drained = rt->drain();
    auto joined = rt->join();
    teardown_ns = now_ns() - s1;
    if (!drained.has_value() || !joined.has_value())
        bench_semantic("runtime drain/join failed");

    // Fail-closed accounting: every counted rep must have moved exactly the
    // whole workload with zero errors and the exact word sum.
    bool all_ok = true;
    for (auto& r : results) {
        if (r.errors != 0 || r.ops != rs.ops || r.bytes != cfg.total_bytes ||
            r.word_sum != rs.expected_sum)
            all_ok = false;
        // U0-A consistency: exactly one operation-CQE lookup per op, all
        // hits, and no control/transport lookups in this no-cancel workload.
        if (r.op_lookup_calls != rs.ops || r.lookup_misses != 0 ||
            r.control_lookup_calls != 0 || r.transport_lookup_calls != 0)
            all_ok = false;
    }

    std::string out;
    out += "{\n";
    out += "  \"bench\": \"tax0u0_router_bench\",\n";
    out += "  \"bench_version\": 1,\n";
    out += "  \"experiment\": \"TAX-0-EXP-U0\",\n";
    out += "  \"backend\": \"uring\",\n";
    out += "  \"real_uring\": true,\n";
    out += std::string("  \"router_scan_mode\": \"") +
           (cfg.scan_mode == ScanMode::forward_ablation ? "forward"
                                                          : "reverse") +
           "\",\n";
    out += "  \"op\": \"read\",\n";
    out += "  \"request_size\": " + std::to_string(cfg.request_size) + ",\n";
    out += "  \"total_bytes\": " + std::to_string(cfg.total_bytes) + ",\n";
    out += "  \"depth\": " + std::to_string(cfg.depth) + ",\n";
    out += "  \"request_capacity\": " + std::to_string(cfg.request_capacity) +
           ",\n";
    out += "  \"uring_queue_depth\": " + std::to_string(cfg.uring_queue_depth) +
           ",\n";
    out += "  \"ops\": " + std::to_string(rs.ops) + ",\n";
    out += "  \"warmup\": " + std::to_string(cfg.warmup) + ",\n";
    out += "  \"reps\": " + std::to_string(cfg.reps) + ",\n";
    out += "  \"expected_word_sum\": " + std::to_string(rs.expected_sum) +
           ",\n";
    {
        std::string esc;
        for (char ch : cfg.file) {
            switch (ch) {
                case '"': esc += "\\\""; break;
                case '\\': esc += "\\\\"; break;
                case '\n': esc += "\\n"; break;
                default: esc += ch;
            }
        }
        out += "  \"file\": \"" + esc + "\",\n";
    }
    out += "  \"lifecycle_setup_ns\": " + std::to_string(setup_ns) + ",\n";
    out += "  \"lifecycle_teardown_ns\": " + std::to_string(teardown_ns) +
           ",\n";
    out += "  \"reps_out\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const RepResult& r = results[i];
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "    {\"wall_ns\": %llu, \"user_ns\": %llu, "
                      "\"sys_ns\": %llu, \"maxrss_kb\": %llu, \"ops\": %llu, "
                      "\"bytes\": %llu, \"word_sum\": %llu, \"errors\": %llu, "
                      "\"op_lookup_calls\": %llu, "
                      "\"op_lookup_iterations_total\": %llu, "
                      "\"op_lookup_iterations_max\": %llu, "
                      "\"control_lookup_calls\": %llu, "
                      "\"transport_lookup_calls\": %llu, "
                      "\"lookup_hits\": %llu, \"lookup_misses\": %llu, "
                      "\"matched_router_index_sum\": %llu, "
                      "\"matched_router_index_max\": %llu}%s\n",
                      (unsigned long long)r.wall_ns,
                      (unsigned long long)r.user_ns,
                      (unsigned long long)r.sys_ns,
                      (unsigned long long)r.maxrss_kb,
                      (unsigned long long)r.ops,
                      (unsigned long long)r.bytes,
                      (unsigned long long)r.word_sum,
                      (unsigned long long)r.errors,
                      (unsigned long long)r.op_lookup_calls,
                      (unsigned long long)r.op_lookup_iterations_total,
                      (unsigned long long)r.op_lookup_iterations_max,
                      (unsigned long long)r.control_lookup_calls,
                      (unsigned long long)r.transport_lookup_calls,
                      (unsigned long long)r.lookup_hits,
                      (unsigned long long)r.lookup_misses,
                      (unsigned long long)r.matched_router_index_sum,
                      (unsigned long long)r.matched_router_index_max,
                      (i + 1 < results.size()) ? "," : "");
        out += buf;
    }
    out += "  ],\n";
    out += "  \"all_reps_ok\": " + std::string(all_ok ? "true" : "false") +
           "\n}\n";
    std::fputs(out.c_str(), stdout);

    if (!all_ok) {
        std::fprintf(stderr,
                     "tax0u0_router_bench: repetition accounting failed "
                     "(ops/bytes/errors/word_sum/scan-witness mismatch)\n");
        ::close(rs.fd);
        return 3;
    }
    ::close(rs.fd);
    return 0;
}

#else

// Stub / production-variant builds: this research binary REQUIRES the
// internal-testing seam + a real liburing ring. Fail closed with a clear
// message instead of benchmarking anything.
int main() {
    std::fprintf(stderr,
                 "tax0u0_router_bench: requires SLUICE_HAS_LIBURING + "
                 "SLUICE_ASYNC_INTERNAL_TESTING (links the "
                 "sluice_async_internal_testing variant; xmake f "
                 "--with-liburing=true)\n");
    return 3;
}

#endif
