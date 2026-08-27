// rx1_workload_bench — RX-1 controlled attribution falsification gate (#234 RX-1).
//
// One deterministic ThreadPool workload driver for the RX-1 experiment. It is
// deliberately SEPARATE from bench/e1_abstraction_tax_bench.cpp: E1 measures
// the abstraction-tax ladder and must keep its original meaning; RX-1 needs a
// single-shape Sluice pipeline with three things E1 does not have:
//
//   1. would_block-aware submission: the application pipeline targets
//      --app-depth in flight; when the RequestArena capacity is below that
//      (intervention I2), a rejected submit is an EXPECTED outcome — the app
//      counts it, awaits the oldest completion to free a slot, and retries.
//      Every rejection is followed by an await: no spin, no sleep.
//   2. an AC-1a observation thread (--observe-interval-ms > 0) that samples
//      the nine public resource accessors SEQUENTIALLY (pull-based,
//      individual calls — intentionally not one atomic snapshot; see
//      research/rx1/RX1_METHOD.md §sampling). sleep_until pacing, never a
//      busy loop; bounded preallocated storage; aggregates only in output.
//   3. process-level OS accounting over the measured window
//      (/proc/self/status context-switch counters and /proc/self/schedstat),
//      which — unlike Sluice internals — is telemetry any external observer
//      of the process could also see, plus getrusage per repetition.
//
// Correctness is fail-closed like E1: every repetition must move exactly
// ops = total_bytes / request_size positional requests; reads must reproduce
// the generator's expected word sum; writes are verified once (untimed) at
// the end; the runtime must drain/join cleanly; exit code 3 on any failure.
//
// Output: one JSON object on stdout (hand-rolled, E1 style). Environment
// fingerprinting, perf, PSI and intervention orchestration live in the
// runner (research/rx1/scripts/rx1.py), not here.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/detail/posix_retry.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <dirent.h>

namespace {

// ---------------------------------------------------------------------------
// Fail-closed helpers (E1 conventions)
// ---------------------------------------------------------------------------

[[noreturn]] void bench_fatal(const char* what, int err) {
    std::fprintf(stderr, "rx1_workload_bench: fatal: %s (errno=%d: %s)\n", what,
                 err, std::strerror(err));
    std::exit(3);
}

[[noreturn]] void bench_semantic(const char* what) {
    std::fprintf(stderr, "rx1_workload_bench: semantic failure: %s\n", what);
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
    if (::getrusage(RUSAGE_SELF, &ru) != 0) bench_fatal("getrusage", errno);
    CpuTime t;
    t.user_ns = static_cast<std::uint64_t>(ru.ru_utime.tv_sec) * 1000000000ull +
                static_cast<std::uint64_t>(ru.ru_utime.tv_usec) * 1000ull;
    t.sys_ns = static_cast<std::uint64_t>(ru.ru_stime.tv_sec) * 1000000000ull +
               static_cast<std::uint64_t>(ru.ru_stime.tv_usec) * 1000ull;
    t.maxrss_kb = static_cast<std::uint64_t>(ru.ru_maxrss);
    return t;
}

// Process-level OS accounting an external observer could equally read:
// voluntary/involuntary context switches and the scheduler wait/run/timeslice
// counters, SUMMED over every thread in /proc/self/task/ — the driving fiber,
// the blocking workers and the observer all live on non-main threads, so the
// leader's /proc/self/status alone would miss almost all activity. The thread
// set is stable inside the measured window (persistent workers; the observer
// reads before starting its own sampling thread), so start/end sums are
// comparable.
struct ProcSelf {
    std::uint64_t ctxt_vol = 0;
    std::uint64_t ctxt_invol = 0;
    std::uint64_t sched_wait_ns = 0;
    std::uint64_t sched_run_ns = 0;
    std::uint64_t sched_slices = 0;
    std::size_t threads = 0;
    bool ok = false;
};

ProcSelf proc_self_now() {
    ProcSelf p;
    if (DIR* d = ::opendir("/proc/self/task")) {
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            std::string task_status =
                std::string("/proc/self/task/") + e->d_name + "/status";
            if (FILE* f = ::fopen(task_status.c_str(), "r")) {
                char line[256];
                while (::fgets(line, sizeof(line), f)) {
                    unsigned long long v = 0;
                    if (std::sscanf(line, "voluntary_ctxt_switches: %llu", &v) == 1)
                        p.ctxt_vol += v;
                    else if (std::sscanf(line, "nonvoluntary_ctxt_switches: %llu",
                                        &v) == 1)
                        p.ctxt_invol += v;
                }
                ::fclose(f);
                ++p.threads;
            }
            std::string ss =
                std::string("/proc/self/task/") + e->d_name + "/schedstat";
            if (FILE* f = ::fopen(ss.c_str(), "r")) {
                unsigned long long a = 0, b = 0, c = 0;
                if (::fscanf(f, "%llu %llu %llu", &a, &b, &c) == 3) {
                    p.sched_wait_ns += a;
                    p.sched_run_ns += b;
                    p.sched_slices += c;
                }
                ::fclose(f);
            }
        }
        ::closedir(d);
        p.ok = true;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Deterministic workload bytes (E1 generator, same contract)
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
    for (std::size_t i = 0; i < kBlock / sizeof(std::uint64_t); ++i) s += words[i];
    return s;
}

void fill_from_master(std::byte* dst, std::size_t len, const std::byte* master) {
    for (std::size_t off = 0; off < len; off += kBlock)
        std::memcpy(dst + off, master, kBlock);
}

std::uint64_t word_sum(const std::byte* p, std::size_t len) {
    auto* w = reinterpret_cast<const std::uint64_t*>(p);
    std::uint64_t s = 0;
    for (std::size_t i = 0; i < len / sizeof(std::uint64_t); ++i) s += w[i];
    return s;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

enum class Op { read, write };

struct Config {
    Op op = Op::read;
    std::size_t request_size = 65536;
    std::size_t total_bytes = 256u << 20;
    std::size_t app_depth = 16;
    std::size_t workers = 4;
    std::size_t capacity = 64;
    std::size_t reps = 3;
    std::size_t warmup = 1;
    bool latency = true;
    unsigned observe_interval_ms = 0;  // 0 = OBS-OFF
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
    if (c.app_depth == 0 || c.app_depth > 1024) {
        err = "--app-depth must be in [1, 1024]";
        return false;
    }
    if (c.workers == 0 || c.workers > 256) {
        err = "--workers must be in [1, 256]";
        return false;
    }
    if (c.capacity == 0 || c.capacity > 1024) {
        err = "--capacity must be in [1, 1024]";
        return false;
    }
    if (c.reps == 0 || c.reps > 1000 || c.warmup > 100) {
        err = "--reps must be >= 1 and --warmup bounded";
        return false;
    }
    constexpr std::size_t kMaxBuf = 1ull << 30;
    if (c.app_depth * c.request_size > kMaxBuf) {
        err = "app-depth * request-size exceeds the 1 GiB buffer budget";
        return false;
    }
    if (c.file.empty()) {
        err = "--file is required";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rep result + run state
// ---------------------------------------------------------------------------

struct RepResult {
    std::uint64_t wall_ns = 0;
    std::uint64_t user_ns = 0;
    std::uint64_t sys_ns = 0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    std::uint64_t word_sum = 0;
    std::uint64_t submit_rejections = 0;
    std::uint64_t errors = 0;
    double lat_p50_ns = 0.0;
    double lat_p95_ns = 0.0;
    double lat_p99_ns = 0.0;
};

struct RunState {
    Config cfg;
    std::vector<std::uint64_t> master_words;
    std::uint64_t expected_sum = 0;
    std::size_t ops = 0;
    int fd = -1;
    std::vector<std::vector<std::byte>> buf;  // app_depth slots
    std::vector<std::uint64_t> lat;           // per-op stamps (latency mode)
    std::vector<sluice::async::Completion<std::size_t>> comp;

    const std::byte* master_bytes() const {
        return reinterpret_cast<const std::byte*>(master_words.data());
    }

    void snapshot_latency(RepResult& r) const {
        if (!cfg.latency) return;
        std::vector<std::uint64_t> srt(lat);
        std::sort(srt.begin(), srt.end());
        auto pct = [&srt](double p) -> double {
            if (srt.empty()) return 0.0;
            std::size_t idx = static_cast<std::size_t>(p / 100.0 * srt.size());
            if (idx >= srt.size()) idx = srt.size() - 1;
            return static_cast<double>(srt[idx]);
        };
        r.lat_p50_ns = pct(50);
        r.lat_p95_ns = pct(95);
        r.lat_p99_ns = pct(99);
    }
};

// ---------------------------------------------------------------------------
// AC-1a observation thread. Sequential individual accessor calls, each row
// stamped; NO combined snapshot exists by design (PR #235 review decision).
// Storage is preallocated and bounded; overflow would stop recording (never
// happens at the protocol's intervals/durations, but is reported honestly).
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxSamples = 4u << 20;

struct ObsSample {
    std::uint64_t t_ns;
    std::size_t slot_in_use;
    std::size_t outstanding;
    std::size_t active_workers;
    std::size_t dispatch_occ;
    std::size_t rejections;  // arena_capacity_rejections() as read at t_ns
};

struct Observer {
    const sluice::async::ThreadPoolBackend* be = nullptr;
    unsigned interval_ms = 0;
    std::atomic<bool> stop{false};
    std::vector<ObsSample> samples;
    std::uint64_t started_ns = 0;
    std::uint64_t stopped_ns = 0;
    std::uint64_t initial_rejections = 0;
    bool overflowed = false;
    std::thread th;

    void start() {
        samples.reserve(1024);
        initial_rejections = be->arena_capacity_rejections();
        started_ns = now_ns();
        th = std::thread([this] { loop(); });
    }

    void loop() {
        using clock = std::chrono::steady_clock;
        auto interval = std::chrono::milliseconds(interval_ms);
        auto next = clock::now() + interval;
        for (;;) {
            std::this_thread::sleep_until(next);
            next += interval;
            if (stop.load(std::memory_order_relaxed)) return;
            if (samples.size() >= kMaxSamples) {
                overflowed = true;
                continue;  // keep pacing; drop rows rather than grow
            }
            ObsSample s;
            s.t_ns = now_ns();
            s.slot_in_use = be->arena_slot_in_use();
            s.outstanding = be->outstanding();
            s.active_workers = be->active_workers();
            s.dispatch_occ = be->dispatch_occupancy();
            s.rejections = be->arena_capacity_rejections();
            samples.push_back(s);
        }
    }

    void join() {
        stop.store(true, std::memory_order_relaxed);
        if (th.joinable()) th.join();
        stopped_ns = now_ns();
    }
};

// Aggregates over the sampled window. Derived quantities are restricted to
// the safe set from the task brief §11 (per-field stats vs immutable
// capacity; never cross-field instantaneous equalities).
struct ObsAggregates {
    std::size_t sample_count = 0;
    double window_s = 0.0;
    double realized_hz = 0.0;
    bool overflowed = false;

    std::size_t arena_capacity = 0;
    std::size_t configured_workers = 0;
    double slot_in_use_max = 0, slot_in_use_mean = 0;
    double outstanding_max = 0, outstanding_mean = 0;
    double active_max = 0, active_mean = 0;
    double dispatch_occ_max = 0, dispatch_occ_mean = 0;
    double frac_slot_at_capacity = 0;
    double frac_active_at_configured = 0;
    double frac_dispatch_nonzero = 0;
    std::size_t arena_high_water_final = 0;
    std::size_t dispatch_high_water_final = 0;
    std::uint64_t rejections_initial = 0;
    std::uint64_t rejections_final = 0;
    std::uint64_t rejections_delta = 0;
};

ObsAggregates aggregate(const Observer& ob) {
    ObsAggregates a;
    a.sample_count = ob.samples.size();
    a.window_s = ob.samples.empty()
                     ? 0.0
                     : static_cast<double>(ob.stopped_ns - ob.started_ns) / 1e9;
    a.realized_hz = a.window_s > 0 ? a.sample_count / a.window_s : 0.0;
    a.overflowed = ob.overflowed;
    const auto n = ob.samples.size();
    if (n != 0 && ob.be != nullptr) {
        a.arena_capacity = ob.be->arena_capacity();
        a.configured_workers = ob.be->configured_worker_count();
        double cap = static_cast<double>(a.arena_capacity);
        double cw = static_cast<double>(a.configured_workers);
        std::uint64_t at_cap = 0, act_eq = 0, disp_nz = 0;
        for (const auto& s : ob.samples) {
            auto smax = [&](double& m, std::size_t v) {
                if (v > m) m = static_cast<double>(v);
            };
            smax(a.slot_in_use_max, s.slot_in_use);
            smax(a.outstanding_max, s.outstanding);
            smax(a.active_max, s.active_workers);
            smax(a.dispatch_occ_max, s.dispatch_occ);
            a.slot_in_use_mean += static_cast<double>(s.slot_in_use);
            a.outstanding_mean += static_cast<double>(s.outstanding);
            a.active_mean += static_cast<double>(s.active_workers);
            a.dispatch_occ_mean += static_cast<double>(s.dispatch_occ);
            if (cap > 0 && static_cast<double>(s.slot_in_use) >= cap) ++at_cap;
            if (cw > 0 && static_cast<double>(s.active_workers) >= cw) ++act_eq;
            if (s.dispatch_occ > 0) ++disp_nz;
        }
        double dn = static_cast<double>(n);
        a.slot_in_use_mean /= dn;
        a.outstanding_mean /= dn;
        a.active_mean /= dn;
        a.dispatch_occ_mean /= dn;
        a.frac_slot_at_capacity = static_cast<double>(at_cap) / dn;
        a.frac_active_at_configured = static_cast<double>(act_eq) / dn;
        a.frac_dispatch_nonzero = static_cast<double>(disp_nz) / dn;
        // Monotone counters read once after the window (sequential, stamped
        // only by "after all samples" — documented as such, not atomic).
        a.arena_high_water_final = ob.be->arena_high_water_mark();
        a.dispatch_high_water_final = ob.be->dispatch_high_water_mark();
    }
    a.rejections_initial = ob.initial_rejections;
    if (ob.be != nullptr) a.rejections_final = ob.be->arena_capacity_rejections();
    a.rejections_delta =
        a.rejections_final > a.rejections_initial
            ? a.rejections_final - a.rejections_initial
            : 0;
    return a;
}

// ---------------------------------------------------------------------------
// File preparation + verification (E1 semantics)
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
        ssize_t n = sluice::detail::retry_on_eintr([&] {
            return ::pread(fd, buf.data(), want, static_cast<off_t>(off));
        });
        if (n < 0) {
            int e = errno;
            ::close(fd);
            bench_fatal("verify pread", e);
        }
        if (n == 0) {
            ::close(fd);
            bench_semantic("verify pread hit EOF before total_bytes");
        }
        sum += word_sum(buf.data(), static_cast<std::size_t>(n));
        off += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (sum != expected_sum)
        bench_semantic("final write verification word-sum mismatch");
}

// ---------------------------------------------------------------------------
// Task body: depth-D submit/await pipeline with would_block retry.
// ---------------------------------------------------------------------------

struct TaskOutcome {
    std::uint64_t word_sum = 0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    std::uint64_t submit_rejections = 0;
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
        // Fill the application's target in-flight window. A would_block
        // rejection (arena capacity < window) is an expected outcome under
        // intervention I2: count it and fall through to the await below so a
        // slot frees; any other rejection is a semantic failure.
        while (submit_k < rs.ops && submit_k - await_k < c.app_depth) {
            std::byte* b = rs.buf[submit_k % c.app_depth].data();
            std::uint64_t off =
                static_cast<std::uint64_t>(submit_k) * c.request_size;
            if (c.op == Op::write)
                fill_from_master(b, c.request_size, rs.master_bytes());
            if (c.latency) rs.lat[submit_k] = now_ns();
            Completion<std::size_t>& cc = rs.comp[submit_k % c.app_depth];
            if (c.op == Op::read) {
                auto sr = ctx.submit_read(ReadOp{rs.fd, b, c.request_size, off}, cc);
                if (!sr.has_value()) {
                    if (sr.error().code == sluice::IoError::Code::would_block) {
                        ++out.submit_rejections;
                        break;
                    }
                    std::snprintf(out.err, sizeof(out.err),
                                  "submit_read rejected (code %d)",
                                  static_cast<int>(sr.error().code));
                    return;
                }
            } else {
                auto sr =
                    ctx.submit_write(WriteOp{rs.fd, b, c.request_size, off}, cc);
                if (!sr.has_value()) {
                    if (sr.error().code == sluice::IoError::Code::would_block) {
                        ++out.submit_rejections;
                        break;
                    }
                    std::snprintf(out.err, sizeof(out.err),
                                  "submit_write rejected (code %d)",
                                  static_cast<int>(sr.error().code));
                    return;
                }
            }
            ++submit_k;
        }
        if (await_k == submit_k)
            bench_semantic("pipeline stall: nothing in flight to await");
        Completion<std::size_t>& cc = rs.comp[await_k % c.app_depth];
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
                word_sum(rs.buf[await_k % c.app_depth].data(), c.request_size);
        if (c.latency) rs.lat[await_k] = now_ns() - rs.lat[await_k];
        cc.reset();
        ++await_k;
        ++out.ops;
        out.bytes += n;
    }
    out.ok = true;
}

RepResult run_rep(sluice::async::ApplicationRuntime& rt, RunState& rs) {
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
    if (!sub.has_value()) bench_semantic("runtime rejected the task admission");
    TaskOutcome out = slot.wait_and_take();
    std::uint64_t t1 = now_ns();
    CpuTime c1 = cpu_time_now();
    r.wall_ns = t1 - t0;
    r.user_ns = c1.user_ns - c0.user_ns;
    r.sys_ns = c1.sys_ns - c0.sys_ns;
    r.word_sum = out.word_sum;
    r.ops = out.ops;
    r.bytes = out.bytes;
    r.submit_rejections = out.submit_rejections;
    if (!out.ok) {
        std::fprintf(stderr, "rx1_workload_bench: task failed: %s\n", out.err);
        r.errors = 1;
    }
    return r;
}

// ---------------------------------------------------------------------------
// CLI + JSON (E1 conventions)
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

bool parse_u(const char* s, unsigned& out) {
    std::size_t v;
    if (!parse_size(s, v) || v > 100000) return false;
    out = static_cast<unsigned>(v);
    return true;
}

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

int usage_error(const char* argv0, const char* detail) {
    std::fprintf(
        stderr,
        "usage: %s --op read|write --file PATH [--request-size N]\n"
        "          [--total-bytes N] [--app-depth N] [--workers N]\n"
        "          [--capacity N] [--reps N] [--warmup N] [--no-latency]\n"
        "          [--observe-interval-ms N]\n"
        "error: %s\n",
        argv0, detail);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
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
        if (a == "--op") {
            std::string s(next("--op"));
            if (s == "read") cfg.op = Op::read;
            else if (s == "write") cfg.op = Op::write;
            else return usage_error(argv[0], "--op must be read|write");
        } else if (a == "--request-size") {
            size_opt("--request-size", cfg.request_size);
        } else if (a == "--total-bytes") {
            size_opt("--total-bytes", cfg.total_bytes);
        } else if (a == "--app-depth") {
            size_opt("--app-depth", cfg.app_depth);
        } else if (a == "--workers") {
            size_opt("--workers", cfg.workers);
        } else if (a == "--capacity") {
            size_opt("--capacity", cfg.capacity);
        } else if (a == "--reps") {
            size_opt("--reps", cfg.reps);
        } else if (a == "--warmup") {
            size_opt("--warmup", cfg.warmup);
        } else if (a == "--no-latency") {
            cfg.latency = false;
        } else if (a == "--observe-interval-ms") {
            if (!parse_u(next("--observe-interval-ms"), cfg.observe_interval_ms))
                return usage_error(argv[0],
                                   "--observe-interval-ms must be 0..100000");
        } else if (a == "--file") {
            cfg.file = next("--file");
        } else {
            return usage_error(argv[0], "unknown argument");
        }
    }
    std::string cfg_err;
    if (!config_valid(cfg, cfg_err)) return usage_error(argv[0], cfg_err.c_str());

    RunState rs;
    rs.cfg = cfg;
    rs.master_words.assign(kBlock / sizeof(std::uint64_t), 0);
    fill_master_block(rs.master_words.data());
    rs.expected_sum =
        block_word_sum(rs.master_words.data()) * (cfg.total_bytes / kBlock);
    rs.ops = cfg.total_bytes / cfg.request_size;
    if (cfg.latency && rs.ops > (32u << 20))
        return usage_error(argv[0], "--latency per-op array exceeds 32M entries");

    prepare_file(cfg, rs);
    int open_flags = cfg.op == Op::read ? O_RDONLY : O_WRONLY;
    rs.fd = ::open(cfg.file.c_str(), open_flags);
    if (rs.fd < 0) bench_fatal("open data file", errno);
    rs.buf.resize(cfg.app_depth);
    for (auto& b : rs.buf) b.assign(cfg.request_size, std::byte{0});
    if (cfg.latency) rs.lat.assign(rs.ops, 0);
    rs.comp = std::vector<sluice::async::Completion<std::size_t>>(cfg.app_depth);

    using namespace sluice::async;
    std::uint64_t setup_ns = 0, teardown_ns = 0;
    // Raw pointer kept beside the unique_ptr handed to the builder: the
    // backend outlives the observer thread, which is joined before runtime
    // teardown. Read-only const accessors only.
    ThreadPoolBackend* be_raw = nullptr;
    std::uint64_t s0 = now_ns();
    {
        ThreadPoolConfig tc;
        tc.request_capacity = cfg.capacity;
        tc.worker_count = cfg.workers;
        auto be = std::make_unique<ThreadPoolBackend>(tc);
        be_raw = be.get();
        RuntimeBuilder builder;
        builder.backend(std::move(be));
        builder.workers(1);  // scheduler workers: DISTINCT resource; single
                             // driving task, matching the E1/apps shape.
        auto built = builder.build();
        if (!built.has_value())
            bench_semantic("RuntimeBuilder::build rejected the config");
        auto rt = std::move(built.value());
        auto started = rt->start();
        if (!started.has_value()) bench_semantic("ApplicationRuntime::start failed");
        setup_ns = now_ns() - s0;

        // Warmup (untimed, unsampled): warms page cache and steady state.
        for (std::size_t r = 0; r < cfg.warmup; ++r) {
            RepResult w = run_rep(*rt, rs);
            if (w.errors != 0) bench_semantic("warmup repetition failed");
        }

        // Measured window: observer + OS accounting around all reps.
        Observer ob;
        ob.be = be_raw;
        ob.interval_ms = cfg.observe_interval_ms;
        ProcSelf p0 = proc_self_now();
        if (cfg.observe_interval_ms != 0) ob.start();

        std::vector<RepResult> results;
        std::uint64_t win_t0 = now_ns();
        for (std::size_t r = 0; r < cfg.reps; ++r) {
            RepResult res = run_rep(*rt, rs);
            rs.snapshot_latency(res);
            results.push_back(res);
        }
        std::uint64_t win_t1 = now_ns();
        if (cfg.observe_interval_ms != 0) ob.join();
        ProcSelf p1 = proc_self_now();
        std::uint64_t app_rejections_total = 0;
        for (auto& r : results) app_rejections_total += r.submit_rejections;

        std::uint64_t s1 = now_ns();
        rt->request_stop();
        auto drained = rt->drain();
        auto joined = rt->join();
        teardown_ns = now_ns() - s1;
        if (!drained.has_value() || !joined.has_value())
            bench_semantic("runtime drain/join failed");

        // Fail-closed accounting over the measured reps.
        bool all_ok = true;
        for (auto& r : results) {
            if (r.errors != 0 || r.ops != rs.ops || r.bytes != cfg.total_bytes ||
                (cfg.op == Op::read && r.word_sum != rs.expected_sum))
                all_ok = false;
        }
        if (cfg.op == Op::write) verify_written_file(cfg, rs.expected_sum);
        if (!all_ok)
            bench_semantic("measured repetition accounting failed "
                           "(ops/bytes/errors/word_sum mismatch)");

        ObsAggregates agg;
        if (cfg.observe_interval_ms != 0) agg = aggregate(ob);

        // ---------------- JSON ----------------
        std::string out;
        out += "{\n";
        out += "  \"bench\": \"rx1_workload_bench\",\n";
        out += "  \"bench_version\": 1,\n";
        out += std::string("  \"op\": \"") +
               (cfg.op == Op::read ? "read" : "write") + "\",\n";
        out += "  \"request_size\": " + std::to_string(cfg.request_size) + ",\n";
        out += "  \"total_bytes\": " + std::to_string(cfg.total_bytes) + ",\n";
        out += "  \"app_depth\": " + std::to_string(cfg.app_depth) + ",\n";
        out += "  \"workers\": " + std::to_string(cfg.workers) + ",\n";
        out += "  \"capacity\": " + std::to_string(cfg.capacity) + ",\n";
        out += "  \"ops_per_rep\": " + std::to_string(rs.ops) + ",\n";
        out += "  \"warmup\": " + std::to_string(cfg.warmup) + ",\n";
        out += "  \"reps\": " + std::to_string(cfg.reps) + ",\n";
        out += "  \"latency\": " + std::string(cfg.latency ? "true" : "false") +
               ",\n";
        {
            std::string esc;
            json_escape(esc, cfg.file);
            out += "  \"file\": \"" + esc + "\",\n";
        }
        out += "  \"observe_interval_ms\": " + std::to_string(cfg.observe_interval_ms) +
               ",\n";
        out += "  \"lifecycle_setup_ns\": " + std::to_string(setup_ns) + ",\n";
        out += "  \"lifecycle_teardown_ns\": " + std::to_string(teardown_ns) + ",\n";
        out += "  \"measured_window_ns\": " + std::to_string(win_t1 - win_t0) +
               ",\n";

        // Workload outcome (visible to any caller of any I/O library).
        out += "  \"outcome\": {\n";
        out += "    \"submit_rejections_total\": " +
               std::to_string(app_rejections_total) + ",\n";
        double thr = 0, lat50 = 0, lat95 = 0, lat99 = 0, usr = 0, sys = 0;
        for (auto& r : results) {
            thr += static_cast<double>(r.bytes);
            lat50 += r.lat_p50_ns;
            lat95 += r.lat_p95_ns;
            lat99 += r.lat_p99_ns;
            usr += static_cast<double>(r.user_ns);
            sys += static_cast<double>(r.sys_ns);
        }
        double dn = static_cast<double>(results.size());
        double wall_med = 0;
        {
            std::vector<std::uint64_t> walls;
            for (auto& r : results) walls.push_back(r.wall_ns);
            std::sort(walls.begin(), walls.end());
            wall_med = static_cast<double>(walls[walls.size() / 2]);
        }
        thr = thr / dn / (wall_med / 1e9) / (1024.0 * 1024.0);
        char nbuf[1024];
        std::snprintf(nbuf, sizeof(nbuf),
                      "\"throughput_mbs_median\": %.3f,\n"
                      "    \"lat_p50_ns_median\": %.1f,\n"
                      "    \"lat_p95_ns_median\": %.1f,\n"
                      "    \"lat_p99_ns_median\": %.1f,\n"
                      "    \"user_ns_sum\": %.0f,\n"
                      "    \"sys_ns_sum\": %.0f",
                      thr, lat50 / dn, lat95 / dn, lat99 / dn, usr, sys);
        out += "    " + std::string(nbuf) + "\n";
        out += "  },\n";

        // External process-level OS accounting over the measured window.
        out += "  \"os_accounting\": {\n";
        std::snprintf(
            nbuf, sizeof(nbuf),
            "\"ctxt_vol_delta\": %llu,\n    \"ctxt_invol_delta\": %llu,\n"
            "    \"sched_wait_ns_delta\": %llu,\n"
            "    \"sched_run_ns_delta\": %llu,\n"
            "    \"sched_slices_delta\": %llu,\n"
            "    \"threads_end\": %llu",
            (unsigned long long)(p1.ctxt_vol - p0.ctxt_vol),
            (unsigned long long)(p1.ctxt_invol - p0.ctxt_invol),
            (unsigned long long)(p1.sched_wait_ns - p0.sched_wait_ns),
            (unsigned long long)(p1.sched_run_ns - p0.sched_run_ns),
            (unsigned long long)(p1.sched_slices - p0.sched_slices),
            (unsigned long long)p1.threads);
        out += "    " + std::string(nbuf) + "\n";
        out += "  },\n";

        // AC-1a observation aggregates (E-only features).
        out += "  \"sluice_obs\": {\n";
        std::snprintf(
            nbuf, sizeof(nbuf),
            "\"sample_count\": %zu,\n    \"window_s\": %.6f,\n"
            "    \"realized_hz\": %.3f,\n    \"overflowed\": %s,\n"
            "    \"arena_capacity\": %zu,\n    \"configured_workers\": %zu,\n"
            "    \"slot_in_use_max\": %.1f,\n    \"slot_in_use_mean\": %.4f,\n"
            "    \"outstanding_max\": %.1f,\n    \"outstanding_mean\": %.4f,\n"
            "    \"active_max\": %.1f,\n    \"active_mean\": %.4f,\n"
            "    \"dispatch_occ_max\": %.1f,\n    \"dispatch_occ_mean\": %.4f,\n"
            "    \"frac_slot_at_capacity\": %.6f,\n"
            "    \"frac_active_at_configured\": %.6f,\n"
            "    \"frac_dispatch_nonzero\": %.6f,\n"
            "    \"arena_high_water_final\": %zu,\n"
            "    \"dispatch_high_water_final\": %zu,\n"
            "    \"rejections_initial\": %llu,\n"
            "    \"rejections_final\": %llu,\n"
            "    \"rejections_delta\": %llu",
            agg.sample_count, agg.window_s, agg.realized_hz,
            agg.overflowed ? "true" : "false", agg.arena_capacity,
            agg.configured_workers, agg.slot_in_use_max, agg.slot_in_use_mean,
            agg.outstanding_max, agg.outstanding_mean, agg.active_max,
            agg.active_mean, agg.dispatch_occ_max, agg.dispatch_occ_mean,
            agg.frac_slot_at_capacity, agg.frac_active_at_configured,
            agg.frac_dispatch_nonzero, agg.arena_high_water_final,
            agg.dispatch_high_water_final,
            (unsigned long long)agg.rejections_initial,
            (unsigned long long)agg.rejections_final,
            (unsigned long long)agg.rejections_delta);
        out += "    " + std::string(nbuf) + "\n";
        out += "  },\n";

        out += "  \"reps_out\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i) {
            const RepResult& r = results[i];
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                          "    {\"wall_ns\": %llu, \"user_ns\": %llu, "
                          "\"sys_ns\": %llu, \"ops\": %llu, \"bytes\": %llu, "
                          "\"word_sum\": %llu, \"submit_rejections\": %llu, "
                          "\"errors\": %llu, \"lat_p50_ns\": %.1f, "
                          "\"lat_p95_ns\": %.1f, \"lat_p99_ns\": %.1f}%s\n",
                          (unsigned long long)r.wall_ns,
                          (unsigned long long)r.user_ns,
                          (unsigned long long)r.sys_ns, (unsigned long long)r.ops,
                          (unsigned long long)r.bytes,
                          (unsigned long long)r.word_sum,
                          (unsigned long long)r.submit_rejections,
                          (unsigned long long)r.errors, r.lat_p50_ns,
                          r.lat_p95_ns, r.lat_p99_ns,
                          (i + 1 < results.size()) ? "," : "");
            out += buf;
        }
        out += "  ],\n";
        out += "  \"all_reps_ok\": " + std::string(all_ok ? "true" : "false") +
               "\n}\n";
        std::fputs(out.c_str(), stdout);

        ::close(rs.fd);
        return all_ok ? 0 : 3;
    }
}
