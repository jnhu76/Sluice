// TAX-0 router-fix candidate shootout (#255) — Layer B: REAL io_uring
// end-to-end shootout. The EXP-0/U0 geometry and machinery (depth-D
// submit/await pipeline over ApplicationRuntime + REAL UringAsyncBackend,
// fail-closed same-work accounting, per-rep scan-iteration + R3 structural
// witness) with ONE selectable variable: the router fix candidate
// (r0 production baseline / r1 reverse scan / r2 low placement + forward
// scan / r3 bounded cookie table), installed through the research-only
// mode seam BEFORE the runtime starts driving the backend.
//
// Differences from tax0u0_router_bench (the #255 frozen design):
//   --op read|write        the WRITE arm validates that the selected
//                          representation is not READ-specific: identical
//                          bytes (splitmix64 master blocks), identical
//                          offsets, exact op/byte accounting, full-file
//                          word-sum verification after the last rep
//                          (mirrors tax0_capacity_bench's write arm);
//                          durability/fsync stays OUTSIDE the measured
//                          router question.
//   --router-fix-mode r0|r1|r2|r3   the candidate (Q == D is enforced by
//                          the runner so dispatch backlog stays out of the
//                          experiment).
//
// This target links sluice_async_internal_testing (NEVER the production
// sluice_async): the whole binary is the research instrument. Production
// Release builds of sluice_async compile none of the seam and keep the
// production router untouched. No public production tuning knob exists.
//
// Output: one JSON object on stdout (consumed by scripts/bench/
// perf-attribution.py `tax0routershootout`, kind tax0routershootout).
// Exit 0 only when every counted repetition completed with exact
// op/byte/word-sum accounting.
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

[[noreturn]] void bench_fatal(const char* what, int err) {
    std::fprintf(stderr, "tax0router_shootout_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(3);
}

[[noreturn]] void bench_semantic(const char* what) {
    std::fprintf(stderr, "tax0router_shootout_bench: semantic failure: %s\n", what);
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
    t.maxrss_kb = static_cast<std::uint64_t>(ru.ru_maxrss);
    return t;
}

// Deterministic workload bytes — IDENTICAL generator to tax0_capacity_bench
// and tax0u0_router_bench (same kSeed, same splitmix64 master block), so
// every candidate consumes byte-identical input at identical offsets.
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

enum class Op { read, write };
enum class FixMode { r0, r1, r2, r3 };

struct Config {
    FixMode fix_mode = FixMode::r0;
    Op op = Op::read;
    std::size_t request_size = 4096;
    std::size_t total_bytes = 128u << 20;
    std::size_t depth = 8;
    std::size_t request_capacity = 8;
    unsigned uring_queue_depth = 8;
    std::size_t reps = 1;
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
    constexpr std::size_t kMaxBuf = 1ull << 30;
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

struct RepResult {
    std::uint64_t wall_ns = 0;
    std::uint64_t user_ns = 0;
    std::uint64_t sys_ns = 0;
    std::uint64_t maxrss_kb = 0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    std::uint64_t word_sum = 0;
    std::uint64_t errors = 0;
    // Structural witness snapshot for exactly this rep (kind-agnostic
    // counters fold inside find_live_router_cookie_ itself).
    std::uint64_t op_lookup_calls = 0;
    std::uint64_t lookup_iterations_total = 0;
    std::uint64_t lookup_iterations_max = 0;
    std::uint64_t control_lookup_calls = 0;
    std::uint64_t transport_lookup_calls = 0;
    std::uint64_t lookup_hits = 0;
    std::uint64_t lookup_misses = 0;
    std::uint64_t matched_router_index_sum = 0;
    std::uint64_t matched_router_index_max = 0;
    std::uint64_t table_insert_calls = 0;
    std::uint64_t table_erase_calls = 0;
    std::uint64_t table_insert_probes_total = 0;
    std::uint64_t table_lookup_probes_total = 0;
    std::uint64_t table_erase_probes_total = 0;
};

struct RunState {
    Config cfg;
    std::vector<std::uint64_t> master_words;
    std::uint64_t expected_sum = 0;
    std::size_t ops = 0;
    int fd = -1;
    std::vector<std::vector<std::byte>> buf;
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

void raw_pread_exact(int fd, std::byte* dst, std::size_t len, std::uint64_t off) {
    ssize_t n = sluice::detail::retry_on_eintr([&] {
        return ::pread(fd, dst, len, static_cast<off_t>(off));
    });
    if (n < 0) bench_fatal("pread", errno);
    if (static_cast<std::size_t>(n) != len)
        bench_semantic("raw pread returned a short read");
}

void prepare_file(const Config& c, const RunState& rs) {
    if (c.op == Op::write) {
        int fd = ::open(c.file.c_str(), O_WRONLY | O_CREAT, 0600);
        if (fd < 0) bench_fatal("open output file", errno);
        if (::ftruncate(fd, static_cast<off_t>(c.total_bytes)) != 0) {
            int e = errno;
            ::close(fd);
            bench_fatal("ftruncate output file", e);
        }
        if (::close(fd) != 0) bench_fatal("close output file", errno);
        return;
    }
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

void verify_written_file(const Config& c, std::uint64_t expected_sum) {
    int fd = ::open(c.file.c_str(), O_RDONLY);
    if (fd < 0) bench_fatal("open output file for verify", errno);
    std::vector<std::byte> buf(1u << 20);
    std::uint64_t sum = 0;
    std::uint64_t off = 0;
    while (off < c.total_bytes) {
        std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(buf.size(), c.total_bytes - off));
        raw_pread_exact(fd, buf.data(), want, off);
        sum += word_sum(buf.data(), want);
        off += want;
    }
    ::close(fd);
    if (sum != expected_sum)
        bench_semantic("final write verification word-sum mismatch");
}

struct TaskOutcome {
    std::uint64_t word_sum = 0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    bool ok = false;
    char err[128] = {0};
};

// One task drives a depth-D submit/await pipeline — IDENTICAL to the
// EXP-0/U0 measured path. The WRITE arm refills each buffer from the
// deterministic master block before every submit (identical bytes across
// candidates), never fsyncs (durability is outside the router question).
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
            if (c.op == Op::write)
                fill_from_master(b, c.request_size, rs.master_bytes());
            Completion<std::size_t>& cc = rs.comp[submit_k % c.depth];
            if (c.op == Op::read) {
                auto sr = ctx.submit_read(ReadOp{rs.fd, b, c.request_size, off}, cc);
                if (!sr.has_value()) {
                    std::snprintf(out.err, sizeof(out.err),
                                  "submit_read rejected (code %d)",
                                  static_cast<int>(sr.error().code));
                    return;
                }
            } else {
                auto sr = ctx.submit_write(WriteOp{rs.fd, b, c.request_size, off}, cc);
                if (!sr.has_value()) {
                    std::snprintf(out.err, sizeof(out.err),
                                  "submit_write rejected (code %d)",
                                  static_cast<int>(sr.error().code));
                    return;
                }
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
        if (c.op == Op::read)
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
    const auto& diag = backend->router_scan_diagnostics_for_test();
    r.op_lookup_calls = diag.operation_cookie_lookup_calls;
    r.lookup_iterations_total = diag.operation_lookup_iterations_total;
    r.lookup_iterations_max = diag.operation_lookup_iterations_max;
    r.control_lookup_calls = diag.control_cookie_lookup_calls;
    r.transport_lookup_calls = diag.transport_cookie_lookup_calls;
    r.lookup_hits = diag.lookup_hits;
    r.lookup_misses = diag.lookup_misses;
    r.matched_router_index_sum = diag.matched_router_index_sum;
    r.matched_router_index_max = diag.matched_router_index_max;
    r.table_insert_calls = diag.table_insert_calls;
    r.table_erase_calls = diag.table_erase_calls;
    r.table_insert_probes_total = diag.table_insert_probes_total;
    r.table_lookup_probes_total = diag.table_lookup_probes_total;
    r.table_erase_probes_total = diag.table_erase_probes_total;
    if (!out.ok) {
        std::fprintf(stderr, "tax0router_shootout_bench: task failed: %s\n",
                     out.err);
        r.errors = 1;
    }
    return r;
}

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
        "usage: %s --file PATH --router-fix-mode r0|r1|r2|r3 [--op read|write]\n"
        "          [--request-size N] [--total-bytes N] [--depth N]\n"
        "          [--request-capacity N] [--uring-queue-depth N]\n"
        "          [--reps N] [--warmup N]\n"
        "note: #255 router-fix candidate shootout. Candidates are research\n"
        "      modes behind SLUICE_ASYNC_INTERNAL_TESTING; Q == D keeps the\n"
        "      dispatch backlog out of the experiment. Requires a real-liburing\n"
        "      build.\n"
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
        if (a == "--router-fix-mode") {
            std::string s(next("--router-fix-mode"));
            if (s == "r0") cfg.fix_mode = FixMode::r0;
            else if (s == "r1") cfg.fix_mode = FixMode::r1;
            else if (s == "r2") cfg.fix_mode = FixMode::r2;
            else if (s == "r3") cfg.fix_mode = FixMode::r3;
            else return usage_error(argv[0],
                                    "--router-fix-mode must be r0|r1|r2|r3");
        } else if (a == "--op") {
            std::string s(next("--op"));
            if (s == "read") cfg.op = Op::read;
            else if (s == "write") cfg.op = Op::write;
            else return usage_error(argv[0], "--op must be read|write");
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

    int open_flags = cfg.op == Op::read ? O_RDONLY : O_WRONLY;
    rs.fd = ::open(cfg.file.c_str(), open_flags);
    if (rs.fd < 0) bench_fatal("open data file", errno);
    rs.init_buffers();

    std::uint64_t setup_ns = 0, teardown_ns = 0;
    std::uint64_t s0 = now_ns();
    std::unique_ptr<ApplicationRuntime> rt;
    UringAsyncBackend* backend = nullptr;
    {
        RuntimeBuilder builder;
        builder.workers(1); // E1-L2 shape: one task drives the pipeline
        auto ub = std::make_unique<UringAsyncBackend>(UringConfig{
            cfg.request_capacity, cfg.uring_queue_depth});
        if (!ub->available()) {
            std::fprintf(stderr,
                         "tax0router_shootout_bench: uring backend did not "
                         "initialize a real ring (available()==false)\n");
            return 3;
        }
        // Install the candidate BEFORE the runtime starts driving the
        // backend (quiescent, single thread).
        const auto mode =
            cfg.fix_mode == FixMode::r1
                ? UringAsyncBackend::RouterFixModeForTest::reverse_scan
            : cfg.fix_mode == FixMode::r2
                ? UringAsyncBackend::RouterFixModeForTest::low_placement_forward
            : cfg.fix_mode == FixMode::r3
                ? UringAsyncBackend::RouterFixModeForTest::bounded_cookie_table
                : UringAsyncBackend::RouterFixModeForTest::production_baseline;
        ub->set_router_fix_mode_for_test(mode);
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

    bool all_ok = true;
    for (auto& r : results) {
        if (r.errors != 0 || r.ops != rs.ops || r.bytes != cfg.total_bytes ||
            (cfg.op == Op::read && r.word_sum != rs.expected_sum))
            all_ok = false;
        // Structural witness shape: exactly one operation-CQE lookup per op,
        // all hits, no control/transport lookups in this no-cancel workload.
        if (r.op_lookup_calls != rs.ops || r.lookup_hits != rs.ops ||
            r.lookup_misses != 0 || r.control_lookup_calls != 0 ||
            r.transport_lookup_calls != 0)
            all_ok = false;
        // R3 must pay table insert+erase exactly once per op; other
        // candidates must not touch the table.
        if (cfg.fix_mode == FixMode::r3 &&
            (r.table_insert_calls != rs.ops || r.table_erase_calls != rs.ops))
            all_ok = false;
        if (cfg.fix_mode != FixMode::r3 &&
            (r.table_insert_calls != 0 || r.table_erase_calls != 0))
            all_ok = false;
        if (r.matched_router_index_max >= cfg.request_capacity)
            all_ok = false;
    }
    if (cfg.op == Op::write) verify_written_file(cfg, rs.expected_sum);

    const char* cand = cfg.fix_mode == FixMode::r0   ? "r0"
                       : cfg.fix_mode == FixMode::r1 ? "r1"
                       : cfg.fix_mode == FixMode::r2 ? "r2"
                                                     : "r3";
    std::string out;
    out += "{\n";
    out += "  \"bench\": \"tax0router_shootout_bench\",\n";
    out += "  \"bench_version\": 1,\n";
    out += "  \"experiment\": \"TAX-0-ROUTER-SHOOTOUT-B\",\n";
    out += "  \"backend\": \"uring\",\n";
    out += "  \"real_uring\": true,\n";
    out += std::string("  \"candidate\": \"") + cand + "\",\n";
    out += std::string("  \"op\": \"") +
           (cfg.op == Op::read ? "read" : "write") + "\",\n";
    out += "  \"request_size\": " + std::to_string(cfg.request_size) + ",\n";
    out += "  \"total_bytes\": " + std::to_string(cfg.total_bytes) + ",\n";
    out += "  \"depth\": " + std::to_string(cfg.depth) + ",\n";
    out += "  \"request_capacity\": " + std::to_string(cfg.request_capacity) + ",\n";
    out += "  \"uring_queue_depth\": " + std::to_string(cfg.uring_queue_depth) + ",\n";
    out += "  \"ops\": " + std::to_string(rs.ops) + ",\n";
    out += "  \"warmup\": " + std::to_string(cfg.warmup) + ",\n";
    out += "  \"reps\": " + std::to_string(cfg.reps) + ",\n";
    out += "  \"expected_word_sum\": " + std::to_string(rs.expected_sum) + ",\n";
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
    out += "  \"lifecycle_teardown_ns\": " + std::to_string(teardown_ns) + ",\n";
    out += "  \"reps_out\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const RepResult& r = results[i];
        char buf[1024];
        const int n = std::snprintf(buf, sizeof(buf),
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
                                    "\"matched_router_index_max\": %llu, "
                                    "\"table_insert_calls\": %llu, "
                                    "\"table_erase_calls\": %llu, "
                                    "\"table_insert_probes_total\": %llu, "
                                    "\"table_lookup_probes_total\": %llu, "
                                    "\"table_erase_probes_total\": %llu}%s\n",
                                    (unsigned long long)r.wall_ns,
                                    (unsigned long long)r.user_ns,
                                    (unsigned long long)r.sys_ns,
                                    (unsigned long long)r.maxrss_kb,
                                    (unsigned long long)r.ops,
                                    (unsigned long long)r.bytes,
                                    (unsigned long long)r.word_sum,
                                    (unsigned long long)r.errors,
                                    (unsigned long long)r.op_lookup_calls,
                                    (unsigned long long)r.lookup_iterations_total,
                                    (unsigned long long)r.lookup_iterations_max,
                                    (unsigned long long)r.control_lookup_calls,
                                    (unsigned long long)r.transport_lookup_calls,
                                    (unsigned long long)r.lookup_hits,
                                    (unsigned long long)r.lookup_misses,
                                    (unsigned long long)r.matched_router_index_sum,
                                    (unsigned long long)r.matched_router_index_max,
                                    (unsigned long long)r.table_insert_calls,
                                    (unsigned long long)r.table_erase_calls,
                                    (unsigned long long)r.table_insert_probes_total,
                                    (unsigned long long)r.table_lookup_probes_total,
                                    (unsigned long long)r.table_erase_probes_total,
                                    (i + 1 < results.size()) ? "," : "");
        if (n < 0 || n >= static_cast<int>(sizeof(buf))) {
            bench_semantic("repetition JSON line truncated (buffer too small)");
        }
        out += buf;
    }
    out += "  ],\n";
    out += "  \"all_reps_ok\": " + std::string(all_ok ? "true" : "false") + "\n}\n";
    std::fputs(out.c_str(), stdout);

    if (!all_ok) {
        std::fprintf(stderr,
                     "tax0router_shootout_bench: repetition accounting failed "
                     "(ops/bytes/word_sum/witness mismatch)\n");
        ::close(rs.fd);
        return 3;
    }
    ::close(rs.fd);
    return 0;
}

#else

int main() {
    std::fprintf(stderr,
                 "tax0router_shootout_bench: requires SLUICE_HAS_LIBURING + "
                 "SLUICE_ASYNC_INTERNAL_TESTING (links "
                 "sluice_async_internal_testing; xmake f --with-liburing=true)\n");
    return 3;
}

#endif
