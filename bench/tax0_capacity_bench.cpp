// TAX-0B / EXP-0 capacity-invariance bench (#250 TAX-0 ladder step 1).
//
// EXPERIMENT QUESTION (preregistered in research/tax0/TAX0-A-HOTPATH-
// TOPOLOGY-AUDIT.md §17 EXP-0, frozen @ 5537187): at a fixed useful
// workload (active depth D, ops, bytes, request size, workers, offsets,
// input data, validation), does increasing UNUSED request capacity C
// increase Sluice user-space cost?
//
//   D fixed = 8;  C ∈ {8, 32, 128, 512};  primary metric instructions:u/op
//   (captured by the runner's perf stat wrapper, one process per measured
//   repetition; this binary reports per-rep wall/user/sys + same-work
//   accounting via getrusage/steady_clock).
//
// This harness INTENTIONALLY decouples request_capacity from depth. The
// existing e1_abstraction_tax_bench keeps request_capacity == depth by
// design (it measures abstraction layers, not capacity); EXP-0 needs the
// two axes independent, so it gets a dedicated binary instead of an E1
// semantic change. e1 remains untouched.
//
// The production path under test is the same E1-L2 shape: ApplicationRuntime
// (scheduler workers = 1) over either REAL backend —
//   ThreadPoolBackend(ThreadPoolConfig{request_capacity=C, worker_count=W})
//   UringAsyncBackend(UringConfig{request_capacity=C, queue_depth=Q})
// — with one task driving a depth-D submit/await pipeline over
// process-lifetime caller-owned buffers + Completions (L7 address
// stability). Fail-closed same-work discipline is inherited from E1: every
// repetition must complete exactly ops = total_bytes / request_size
// positional requests with the exact expected 8-byte word sum (READ) or a
// full-file verification after the last repetition (WRITE); short I/O,
// zero-progress writes, completion mismatches, capacity < depth (the
// pipeline would exceed the arena -> would_block), and a non-real uring
// ring all abort with exit code 3. Never a sentinel performance value.
//
// Output: one JSON object on stdout (consumed by scripts/bench/
// perf-attribution.py `tax0`, which merges perf counters and produces the
// evidence artifact). Exit 0 only when every counted repetition completed
// with exact op/byte accounting.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>
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

namespace {

// ---------------------------------------------------------------------------
// Fail-closed helpers
// ---------------------------------------------------------------------------

[[noreturn]] void bench_fatal(const char* what, int err) {
    std::fprintf(stderr, "tax0_capacity_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(3);
}

[[noreturn]] void bench_semantic(const char* what) {
    std::fprintf(stderr, "tax0_capacity_bench: semantic failure: %s\n", what);
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
// e1_abstraction_tax_bench (same kSeed, same splitmix64 master block), so
// EXP-0 cells share the exact input pattern family with the E1 baseline
// and every C cell within this bench shares byte-identical input.
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
// Raw positional I/O (file preparation / verification only — the measured
// path always goes through the production backend).
// ---------------------------------------------------------------------------

void raw_pread_exact(int fd, std::byte* dst, std::size_t len,
                     std::uint64_t off) {
    ssize_t n = sluice::detail::retry_on_eintr([&] {
        return ::pread(fd, dst, len, static_cast<off_t>(off));
    });
    if (n < 0) bench_fatal("pread", errno);
    if (static_cast<std::size_t>(n) != len)
        bench_semantic("raw pread returned a short read");
}

// ---------------------------------------------------------------------------
// Configuration + validation
// ---------------------------------------------------------------------------

enum class Op { read, write };
enum class Backend { threadpool, uring };

struct Config {
    Backend backend = Backend::threadpool;
    Op op = Op::read;
    std::size_t request_size = 4096;
    std::size_t total_bytes = 256u << 20;
    std::size_t depth = 8;
    std::size_t request_capacity = 8;
    std::size_t workers = 1;          // ThreadPoolBackend worker_count
    unsigned uring_queue_depth = 8;   // io_uring SQ/CQ depth (kernel-owned)
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
    // The depth-D pipeline needs D simultaneous arena slots; capacity below
    // depth would turn steady state into would_block refusals — that is a
    // different experiment (backpressure), not capacity invariance.
    if (c.request_capacity < c.depth || c.request_capacity > 65536) {
        err = "--request-capacity must be in [depth, 65536]";
        return false;
    }
    if (c.workers == 0 || c.workers > 256) {
        err = "--workers must be in [1, 256]";
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
// File preparation (untimed) + final write verification (untimed, once)
// — identical semantics to e1_abstraction_tax_bench.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// The measured path: one task drives a depth-D submit/await pipeline
// (E1-L2 shape). Per-op work beyond I/O is ladder-invariant: the byte step
// (read word-sum / write master-block fill) only.
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
            if (c.op == Op::write)
                fill_from_master(b, c.request_size, rs.master_bytes());
            Completion<std::size_t>& cc = rs.comp[submit_k % c.depth];
            if (c.op == Op::read) {
                auto sr =
                    ctx.submit_read(ReadOp{rs.fd, b, c.request_size, off}, cc);
                if (!sr.has_value()) {
                    std::snprintf(out.err, sizeof(out.err),
                                  "submit_read rejected (code %d)",
                                  static_cast<int>(sr.error().code));
                    return;
                }
            } else {
                auto sr = ctx.submit_write(
                    WriteOp{rs.fd, b, c.request_size, off}, cc);
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

RepResult run_one_rep(sluice::async::ApplicationRuntime& rt, RunState& rs) {
    using namespace sluice::async;
    RepResult r;
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
    if (!out.ok) {
        std::fprintf(stderr, "tax0_capacity_bench: task failed: %s\n",
                     out.err);
        r.errors = 1;
    }
    return r;
}

// ---------------------------------------------------------------------------
// JSON emission (hand-rolled; every emitted string is a path we control —
// escaped anyway).
// ---------------------------------------------------------------------------

void json_escape(std::string& out, const std::string& s) {
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default: out += ch;
        }
    }
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
        "usage: %s --backend threadpool|uring --file PATH [--op read|write]\n"
        "          [--request-size N] [--total-bytes N] [--depth N]\n"
        "          [--request-capacity N] [--workers N]\n"
        "          [--uring-queue-depth N] [--reps N] [--warmup N]\n"
        "note: --request-capacity is INDEPENDENT of --depth (EXP-0); it\n"
        "      must be >= depth. --workers is ThreadPoolBackend only;\n"
        "      --uring-queue-depth is the io_uring SQ/CQ depth (uring\n"
        "      only; requires a real-liburing build).\n"
        "error: %s\n",
        argv0, detail);
    return 2;
}

}  // namespace

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
        if (a == "--backend") {
            std::string s(next("--backend"));
            if (s == "threadpool") cfg.backend = Backend::threadpool;
            else if (s == "uring") cfg.backend = Backend::uring;
            else return usage_error(argv[0],
                                    "--backend must be threadpool|uring");
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
        } else if (a == "--workers") {
            size_opt("--workers", cfg.workers);
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

    // uring arm requires a REAL liburing ring — fail closed before any
    // measurement rather than silently benchmarking the stub.
    bool real_uring = false;
    if (cfg.backend == Backend::uring) {
#if defined(SLUICE_HAS_LIBURING)
        // capability probe happens on the real backend instance below
        real_uring = true;
#else
        std::fprintf(stderr,
                     "tax0_capacity_bench: --backend uring requires a "
                     "SLUICE_HAS_LIBURING build (xmake f --with-liburing=true)"
                     "\n");
        return 3;
#endif
    }

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

    // Runtime lifecycle is measured OUTSIDE the per-rep windows: build +
    // start before the first rep, request_stop/drain/join after the last.
    std::uint64_t setup_ns = 0, teardown_ns = 0;
    std::uint64_t s0 = now_ns();
    std::unique_ptr<ApplicationRuntime> rt;
    {
        RuntimeBuilder builder;
        builder.workers(1);  // scheduler workers: one task drives the
                             // pipeline (E1-L2 shape; the EXP-0 axis is
                             // backend capacity, not scheduler width)
        if (cfg.backend == Backend::threadpool) {
            ThreadPoolConfig tc;
            tc.request_capacity = cfg.request_capacity;
            tc.worker_count = cfg.workers;
            builder.backend(std::make_unique<ThreadPoolBackend>(tc));
        } else {
#if defined(SLUICE_HAS_LIBURING)
            auto ub = std::make_unique<UringAsyncBackend>(UringConfig{
                cfg.request_capacity, cfg.uring_queue_depth});
            if (!ub->available()) {
                std::fprintf(stderr,
                             "tax0_capacity_bench: uring backend did not "
                             "initialize a real ring (available()==false)\n");
                return 3;
            }
            builder.backend(std::move(ub));
#endif
        }
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
        RepResult res = run_one_rep(*rt, rs);
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
    // whole workload with zero errors, and (read) the exact word sum.
    bool all_ok = true;
    for (auto& r : results) {
        if (r.errors != 0 || r.ops != rs.ops || r.bytes != cfg.total_bytes ||
            (cfg.op == Op::read && r.word_sum != rs.expected_sum))
            all_ok = false;
    }
    if (cfg.op == Op::write) verify_written_file(cfg, rs.expected_sum);

    std::string out;
    out += "{\n";
    out += "  \"bench\": \"tax0_capacity_bench\",\n";
    out += "  \"bench_version\": 1,\n";
    out += "  \"experiment\": \"TAX-0B-EXP0\",\n";
    out += std::string("  \"backend\": \"") +
           (cfg.backend == Backend::threadpool ? "threadpool" : "uring") +
           "\",\n";
    out += std::string("  \"real_uring\": ") +
           (cfg.backend == Backend::uring && real_uring ? "true" : "false") +
           ",\n";
    out += std::string("  \"op\": \"") +
           (cfg.op == Op::read ? "read" : "write") + "\",\n";
    out += "  \"request_size\": " + std::to_string(cfg.request_size) + ",\n";
    out += "  \"total_bytes\": " + std::to_string(cfg.total_bytes) + ",\n";
    out += "  \"depth\": " + std::to_string(cfg.depth) + ",\n";
    out += "  \"request_capacity\": " + std::to_string(cfg.request_capacity) +
           ",\n";
    out += "  \"workers\": " + std::to_string(cfg.workers) + ",\n";
    out += "  \"uring_queue_depth\": " + std::to_string(cfg.uring_queue_depth) +
           ",\n";
    out += "  \"ops\": " + std::to_string(rs.ops) + ",\n";
    out += "  \"warmup\": " + std::to_string(cfg.warmup) + ",\n";
    out += "  \"reps\": " + std::to_string(cfg.reps) + ",\n";
    out += "  \"expected_word_sum\": " + std::to_string(rs.expected_sum) +
           ",\n";
    {
        std::string esc;
        json_escape(esc, cfg.file);
        out += "  \"file\": \"" + esc + "\",\n";
    }
    out += "  \"lifecycle_setup_ns\": " + std::to_string(setup_ns) + ",\n";
    out += "  \"lifecycle_teardown_ns\": " + std::to_string(teardown_ns) +
           ",\n";
    out += "  \"reps_out\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const RepResult& r = results[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "    {\"wall_ns\": %llu, \"user_ns\": %llu, "
                      "\"sys_ns\": %llu, \"maxrss_kb\": %llu, \"ops\": %llu, "
                      "\"bytes\": %llu, \"word_sum\": %llu, \"errors\": %llu}"
                      "%s\n",
                      (unsigned long long)r.wall_ns,
                      (unsigned long long)r.user_ns,
                      (unsigned long long)r.sys_ns,
                      (unsigned long long)r.maxrss_kb,
                      (unsigned long long)r.ops,
                      (unsigned long long)r.bytes,
                      (unsigned long long)r.word_sum,
                      (unsigned long long)r.errors,
                      (i + 1 < results.size()) ? "," : "");
        out += buf;
    }
    out += "  ],\n";
    out += "  \"all_reps_ok\": " + std::string(all_ok ? "true" : "false") +
           "\n}\n";
    std::fputs(out.c_str(), stdout);

    if (!all_ok) {
        std::fprintf(stderr,
                     "tax0_capacity_bench: repetition accounting failed "
                     "(ops/bytes/errors%s mismatch)\n",
                     cfg.op == Op::read ? "/word_sum" : "");
        ::close(rs.fd);
        return 3;
    }
    ::close(rs.fd);
    return 0;
}
