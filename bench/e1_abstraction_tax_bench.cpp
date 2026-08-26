// E1 explicit-I/O abstraction-tax ladder (#221 G0 — Core Cost baseline).
//
// One binary measures three execution layers over the SAME deterministic
// operation stream, so Cost(Ln) - Cost(Ln-1) isolates what layer n adds:
//
//   L0_raw    raw blocking pread/pwrite at parallelism D
//             (D == 1: inline serial loop; D > 1: D strided raw threads,
//              no queue, no handoff — the OS syscall-stream floor)
//   L1_pool   minimal competent fixed std::thread pool (W persistent
//             workers, mutex+condvar bounded ring of capacity D) running
//             direct pread/pwrite — OS/thread-pool execution cost
//   L2_sluice the real public path: ApplicationRuntime + ThreadPoolBackend
//             (request_capacity == D, worker_count == W, scheduler workers
//             1), one task driving a depth-D Completion pipeline — the
//             explicit-I/O control plane
//
// Tax definitions (docs/verification/explicit-io-abstraction-tax.md):
//   ThreadPool direct tax   = T_L1 - T_L0
//   Sluice incremental tax  = T_L2 - T_L1     (NOT T_L2 - T_L0: L1 already
//                                              contains concurrent execution
//                                              machinery)
//
// Same-work guarantee (fail-closed): every ladder processes exactly
// ops = total_bytes / request_size positional requests of request_size
// bytes over the same file bytes. READ consumes each returned buffer with
// an 8-byte word sum that must equal the generator's expected sum on every
// repetition; WRITE fills each submitted buffer from the same deterministic
// master block and the final file is read back and verified once (untimed)
// after the last repetition. Short reads, short writes, unexpected EOF, I/O
// errors, and completion mismatches abort with exit code 3 — never a
// sentinel row.
//
// Timing scope: per-repetition wall_ns is the STEADY-STATE op stream
// (persistent workers / persistent Runtime created before the first rep).
// One-time ladder construction (thread spawn / Runtime build+start) and
// teardown (join / request_stop+drain+join) are measured separately as
// lifecycle_setup_ns / lifecycle_teardown_ns and are NOT part of wall_ns;
// both are recorded so neither cost is hidden or conflated.
//
// Latency instrumentation is opt-in (--latency): preallocated per-op
// submit->completion stamps (no per-op allocation). L0 measures the syscall
// as seen by the issuing thread; L1/L2 measure submit -> completion as
// observed by the submitting code (queue wait included — that is the
// latency the driving thread experiences). In throughput mode the only
// per-op work is the ladder-invariant byte step (read word-sum / write
// master-block fill), identical arithmetic in every ladder.
//
// Output: one JSON object on stdout. Exit 0 only when every repetition
// completed with exact op/byte accounting and (read) exact word sums.
// Results are environment-sensitive; no universal performance claim.
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
#include <condition_variable>
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

namespace {

// ---------------------------------------------------------------------------
// Fail-closed helpers
// ---------------------------------------------------------------------------

[[noreturn]] void bench_fatal(const char* what, int err) {
    std::fprintf(stderr, "e1_abstraction_tax_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(3);
}

[[noreturn]] void bench_semantic(const char* what) {
    std::fprintf(stderr, "e1_abstraction_tax_bench: semantic failure: %s\n",
                 what);
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
// Deterministic workload bytes. One 4 KiB master block of splitmix64 words;
// every file/buffer fill is a memcpy of that block (fill runs at memcpy
// speed, never the bottleneck), and the expected whole-file word sum has a
// closed form: block_sum * block_count.
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

// Fill `len` bytes (a whole number of blocks) from the master block.
void fill_from_master(std::byte* dst, std::size_t len, const std::byte* master) {
    for (std::size_t off = 0; off < len; off += kBlock)
        std::memcpy(dst + off, master, kBlock);
}

// 8-byte word sum over `len` bytes (request/total sizes are block-aligned,
// hence word-aligned; vector storage is operator-new aligned).
std::uint64_t word_sum(const std::byte* p, std::size_t len) {
    auto* w = reinterpret_cast<const std::uint64_t*>(p);
    std::uint64_t s = 0;
    for (std::size_t i = 0; i < len / sizeof(std::uint64_t); ++i)
        s += w[i];
    return s;
}

// ---------------------------------------------------------------------------
// Raw positional I/O with repository retry authority + fail-closed
// semantics. A short read (or zero-progress / short write) is a semantic
// failure, not data.
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

void raw_pwrite_exact(int fd, const std::byte* src, std::size_t len,
                      std::uint64_t off) {
    ssize_t n = sluice::detail::retry_on_eintr([&] {
        return ::pwrite(fd, src, len, static_cast<off_t>(off));
    });
    if (n < 0) bench_fatal("pwrite", errno);
    if (n == 0) bench_semantic("raw pwrite made zero progress");
    if (static_cast<std::size_t>(n) != len)
        bench_semantic("raw pwrite returned a short write");
}

// ---------------------------------------------------------------------------
// Configuration + validation
// ---------------------------------------------------------------------------

enum class Op { read, write };
enum class Ladder { L0_raw, L1_pool, L2_sluice };

const char* ladder_name(Ladder l) {
    switch (l) {
        case Ladder::L0_raw: return "L0_raw";
        case Ladder::L1_pool: return "L1_pool";
        case Ladder::L2_sluice: return "L2_sluice";
    }
    return "?";
}

struct Config {
    Ladder ladder = Ladder::L0_raw;
    Op op = Op::read;
    std::size_t request_size = 4096;
    std::size_t total_bytes = 256u << 20;
    std::size_t depth = 1;
    std::size_t workers = 1;
    std::size_t reps = 7;
    std::size_t warmup = 1;
    bool latency = false;
    std::string file;
};

// Guards: request/total geometry must be exact, and buffer memory
// (depth * request_size) stays bounded.
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
    if (c.workers == 0 || c.workers > 256) {
        err = "--workers must be in [1, 256]";
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
// Shared run state (per process): buffers, L2 Completions, latency stamps
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
    double lat_p50_ns = 0.0;
    double lat_p95_ns = 0.0;
    double lat_p99_ns = 0.0;
};

struct RunState {
    Config cfg;
    std::vector<std::uint64_t> master_words;  // kBlock/8 splitmix64 words
    std::uint64_t expected_sum = 0;
    std::size_t ops = 0;
    int fd = -1;
    std::vector<std::vector<std::byte>> buf;  // depth slots x request_size
    std::vector<std::uint64_t> lat;           // per-op stamps (latency mode)
    // L2 pipeline Completions: process-lifetime, address-stable (L7),
    // default-constructed in place (non-movable by contract).
    std::vector<sluice::async::Completion<std::size_t>> l2_comp;

    const std::byte* master_bytes() const {
        return reinterpret_cast<const std::byte*>(master_words.data());
    }

    void init_buffers() {
        buf.resize(cfg.depth);
        for (auto& b : buf) b.assign(cfg.request_size, std::byte{0});
        if (cfg.latency) lat.assign(ops, 0);
    }

    // Snapshot this rep's per-op latencies into percentiles before the next
    // rep overwrites the stamps.
    void snapshot_latency(RepResult& r) const {
        if (!cfg.latency) return;
        std::vector<std::uint64_t> srt(lat);
        std::sort(srt.begin(), srt.end());
        auto pct = [&srt](double p) -> double {
            if (srt.empty()) return 0.0;
            std::size_t idx =
                static_cast<std::size_t>(p / 100.0 * srt.size());
            if (idx >= srt.size()) idx = srt.size() - 1;
            return static_cast<double>(srt[idx]);
        };
        r.lat_p50_ns = pct(50);
        r.lat_p95_ns = pct(95);
        r.lat_p99_ns = pct(99);
    }
};

// ---------------------------------------------------------------------------
// File preparation (untimed) + final write verification (untimed, once)
// ---------------------------------------------------------------------------

// READ: generate the deterministic pattern file only when absent or wrongly
// sized (a re-run over an existing file reuses page-cache-warm bytes).
// WRITE: pre-size the output file so every pwrite lands inside an existing
// file, matching the read geometry.
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
    // 1 MiB staging chunk (256 master blocks): sane syscall count while
    // staying cache-friendly.
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
// L0 — raw blocking pread/pwrite.
// D == 1: inline serial loop on the main thread (zero synchronization — the
// purest syscall stream). D > 1: D persistent raw threads, static stride
// partition (thread t handles ops t, t+D, ...), start barrier + done CV.
// ---------------------------------------------------------------------------

struct L0Threads {
    std::mutex m;
    std::condition_variable start_cv;
    std::condition_variable done_cv;
    std::uint64_t gen = 0;
    std::size_t done = 0;
    bool stop = false;
    std::vector<std::thread> threads;
    RunState* rs = nullptr;
    std::atomic<std::uint64_t> sum_rep{0};

    void worker(std::size_t t) {
        std::uint64_t seen_gen = 0;
        for (;;) {
            std::unique_lock<std::mutex> lk(m);
            start_cv.wait(lk, [&] { return gen != seen_gen || stop; });
            if (stop) return;
            seen_gen = gen;
            lk.unlock();
            const Config& c = rs->cfg;
            std::uint64_t local_sum = 0;
            std::byte* b = rs->buf[t].data();
            for (std::size_t k = t; k < rs->ops; k += c.depth) {
                std::uint64_t off =
                    static_cast<std::uint64_t>(k) * c.request_size;
                std::uint64_t t0 = 0;
                if (rs->cfg.latency) t0 = now_ns();
                if (c.op == Op::read) {
                    raw_pread_exact(rs->fd, b, c.request_size, off);
                    local_sum += word_sum(b, c.request_size);
                } else {
                    fill_from_master(b, c.request_size, rs->master_bytes());
                    raw_pwrite_exact(rs->fd, b, c.request_size, off);
                }
                if (rs->cfg.latency) rs->lat[k] = now_ns() - t0;
            }
            sum_rep.fetch_add(local_sum, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk2(m);
            ++done;
            done_cv.notify_all();
        }
    }
};

RepResult run_l0_inline(RunState& rs) {
    const Config& c = rs.cfg;
    RepResult r;
    CpuTime c0 = cpu_time_now();
    std::uint64_t t0 = now_ns();
    std::byte* b = rs.buf[0].data();
    for (std::size_t k = 0; k < rs.ops; ++k) {
        std::uint64_t off = static_cast<std::uint64_t>(k) * c.request_size;
        std::uint64_t s0 = 0;
        if (c.latency) s0 = now_ns();
        if (c.op == Op::read) {
            raw_pread_exact(rs.fd, b, c.request_size, off);
            r.word_sum += word_sum(b, c.request_size);
        } else {
            fill_from_master(b, c.request_size, rs.master_bytes());
            raw_pwrite_exact(rs.fd, b, c.request_size, off);
        }
        if (c.latency) rs.lat[k] = now_ns() - s0;
        ++r.ops;
        r.bytes += c.request_size;
    }
    std::uint64_t t1 = now_ns();
    CpuTime c1 = cpu_time_now();
    r.wall_ns = t1 - t0;
    r.user_ns = c1.user_ns - c0.user_ns;
    r.sys_ns = c1.sys_ns - c0.sys_ns;
    r.maxrss_kb = c1.maxrss_kb;
    return r;
}

RepResult run_l0_parallel_rep(L0Threads& pool) {
    RepResult r;
    CpuTime c0 = cpu_time_now();
    pool.sum_rep.store(0, std::memory_order_relaxed);
    std::uint64_t t0;
    {
        std::lock_guard<std::mutex> lk(pool.m);
        pool.done = 0;
        ++pool.gen;
        t0 = now_ns();
    }
    pool.start_cv.notify_all();
    {
        std::unique_lock<std::mutex> lk(pool.m);
        pool.done_cv.wait(
            lk, [&] { return pool.done == pool.rs->cfg.depth; });
    }
    std::uint64_t t1 = now_ns();
    CpuTime c1 = cpu_time_now();
    r.wall_ns = t1 - t0;
    r.user_ns = c1.user_ns - c0.user_ns;
    r.sys_ns = c1.sys_ns - c0.sys_ns;
    r.maxrss_kb = c1.maxrss_kb;
    r.word_sum = pool.sum_rep.load(std::memory_order_relaxed);
    r.ops = pool.rs->ops;
    r.bytes = pool.rs->cfg.total_bytes;
    return r;
}

// ---------------------------------------------------------------------------
// L1 — minimal competent fixed std::thread pool.
// W persistent workers; one mutex; work_cv (workers) + done_cv (producer);
// bounded job ring of capacity D (the offered window); per-slot done flags.
// No per-request heap allocation; proper join. The producer enforces the
// depth-D in-flight window by observing the oldest slot's done flag.
// ---------------------------------------------------------------------------

struct L1Pool {
    std::mutex m;
    std::condition_variable work_cv;
    std::condition_variable done_cv;
    std::vector<std::size_t> ring;  // pending op indices, capacity depth
    std::size_t head = 0, count = 0;
    std::vector<bool> done_flag;  // [slot]
    bool stop = false;
    std::vector<std::thread> threads;
    RunState* rs = nullptr;
    std::atomic<std::uint64_t> sum_rep{0};

    void worker() {
        for (;;) {
            std::size_t k;
            {
                std::unique_lock<std::mutex> lk(m);
                work_cv.wait(lk, [&] { return stop || count > 0; });
                if (stop && count == 0) return;
                k = ring[head];
                head = (head + 1) % rs->cfg.depth;
                --count;
            }
            const Config& c = rs->cfg;
            std::byte* b = rs->buf[k % c.depth].data();
            std::uint64_t off =
                static_cast<std::uint64_t>(k) * c.request_size;
            if (c.op == Op::read) {
                raw_pread_exact(rs->fd, b, c.request_size, off);
                sum_rep.fetch_add(word_sum(b, c.request_size),
                                  std::memory_order_relaxed);
            } else {
                raw_pwrite_exact(rs->fd, b, c.request_size, off);
            }
            {
                std::lock_guard<std::mutex> lk(m);
                done_flag[k % c.depth] = true;
            }
            done_cv.notify_all();
        }
    }
};

RepResult run_l1_rep(L1Pool& pool) {
    RunState& rs = *pool.rs;
    const Config& c = rs.cfg;
    RepResult r;
    CpuTime c0 = cpu_time_now();
    pool.sum_rep.store(0, std::memory_order_relaxed);
    std::size_t await_ptr = 0;  // oldest not-yet-observed op
    std::uint64_t t0 = now_ns();
    for (std::size_t k = 0; k < rs.ops; ++k) {
        if (k - await_ptr == c.depth) {
            std::unique_lock<std::mutex> lk(pool.m);
            pool.done_cv.wait(
                lk, [&] { return pool.done_flag[await_ptr % c.depth]; });
            pool.done_flag[await_ptr % c.depth] = false;
            lk.unlock();
            if (c.latency)
                rs.lat[await_ptr] = now_ns() - rs.lat[await_ptr];
            ++await_ptr;
        }
        if (c.op == Op::write)
            fill_from_master(rs.buf[k % c.depth].data(), c.request_size,
                             rs.master_bytes());
        if (c.latency) rs.lat[k] = now_ns();  // start stamp
        {
            std::lock_guard<std::mutex> lk(pool.m);
            pool.ring[(pool.head + pool.count) % c.depth] = k;
            ++pool.count;
        }
        pool.work_cv.notify_one();
    }
    while (await_ptr < rs.ops) {
        std::unique_lock<std::mutex> lk(pool.m);
        pool.done_cv.wait(
            lk, [&] { return pool.done_flag[await_ptr % c.depth]; });
        pool.done_flag[await_ptr % c.depth] = false;
        lk.unlock();
        if (c.latency) rs.lat[await_ptr] = now_ns() - rs.lat[await_ptr];
        ++await_ptr;
    }
    std::uint64_t t1 = now_ns();
    CpuTime c1 = cpu_time_now();
    r.wall_ns = t1 - t0;
    r.user_ns = c1.user_ns - c0.user_ns;
    r.sys_ns = c1.sys_ns - c0.sys_ns;
    r.maxrss_kb = c1.maxrss_kb;
    r.word_sum = pool.sum_rep.load(std::memory_order_relaxed);
    r.ops = rs.ops;
    r.bytes = c.total_bytes;
    return r;
}

// ---------------------------------------------------------------------------
// L2 — the real public Sluice path.
// ApplicationRuntime + ThreadPoolBackend(request_capacity == depth,
// worker_count == workers) built once per process; per rep one task is
// admitted that drives a depth-D submit/await pipeline over caller-owned
// process-lifetime buffers + Completions (L7 address stability), the
// sluice-copy Version B pipeline shape. Runtime lifecycle (build/start and
// request_stop/drain/join) is measured separately, outside wall_ns.
// ---------------------------------------------------------------------------

struct L2Runtime {
    std::unique_ptr<sluice::async::ApplicationRuntime> rt;
    std::uint64_t setup_ns = 0;
    std::uint64_t teardown_ns = 0;
};

struct L2TaskOutcome {
    std::uint64_t word_sum = 0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    bool ok = false;
    char err[128] = {0};
};

void l2_task_body(sluice::async::RuntimeTaskContext& ctx, RunState& rs,
                  L2TaskOutcome& out) {
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
            if (c.latency) rs.lat[submit_k] = now_ns();  // start stamp
            Completion<std::size_t>& cc = rs.l2_comp[submit_k % c.depth];
            if (c.op == Op::read) {
                auto sr =
                    ctx.submit_read(ReadOp{rs.fd, b, c.request_size, off}, cc);
                if (!sr.has_value()) {
                    std::snprintf(out.err, sizeof(out.err),
                                  "L2 submit_read rejected (code %d)",
                                  static_cast<int>(sr.error().code));
                    return;
                }
            } else {
                auto sr = ctx.submit_write(
                    WriteOp{rs.fd, b, c.request_size, off}, cc);
                if (!sr.has_value()) {
                    std::snprintf(out.err, sizeof(out.err),
                                  "L2 submit_write rejected (code %d)",
                                  static_cast<int>(sr.error().code));
                    return;
                }
            }
            ++submit_k;
        }
        Completion<std::size_t>& cc = rs.l2_comp[await_k % c.depth];
        auto wr = ctx.await_completion(cc);
        if (!wr.has_value()) {
            std::snprintf(out.err, sizeof(out.err),
                          "L2 await_completion rejected (code %d)",
                          static_cast<int>(wr.error().code));
            return;
        }
        auto res = cc.result();
        if (!res.has_value()) {
            std::snprintf(out.err, sizeof(out.err),
                          "L2 terminal I/O error (code %d, os %d)",
                          static_cast<int>(res.error().code),
                          res.error().os_errno);
            return;
        }
        std::size_t n = res.value();
        if (n != c.request_size) {
            std::snprintf(out.err, sizeof(out.err),
                          "L2 op %zu moved %zu bytes (want %zu)", await_k, n,
                          c.request_size);
            return;
        }
        if (c.op == Op::read)
            out.word_sum +=
                word_sum(rs.buf[await_k % c.depth].data(), c.request_size);
        if (c.latency) rs.lat[await_k] = now_ns() - rs.lat[await_k];
        cc.reset();
        ++await_k;
        ++out.ops;
        out.bytes += n;
    }
    out.ok = true;
}

RepResult run_l2_rep(L2Runtime& l2, RunState& rs) {
    using namespace sluice::async;
    RepResult r;
    CpuTime c0 = cpu_time_now();
    std::uint64_t t0 = now_ns();
    TaskResultSlot<L2TaskOutcome> slot;
    auto sub = l2.rt->submit([&rs, &slot](RuntimeTaskContext& ctx) {
        L2TaskOutcome out;
        l2_task_body(ctx, rs, out);
        slot.publish(std::move(out));
    });
    if (!sub.has_value())
        bench_semantic("L2 runtime rejected the task admission");
    L2TaskOutcome out = slot.wait_and_take();
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
        std::fprintf(stderr, "e1_abstraction_tax_bench: L2 task failed: %s\n",
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
        "usage: %s --ladder L0|L1|L2 --op read|write --file PATH\n"
        "          [--request-size N] [--total-bytes N] [--depth N]\n"
        "          [--workers N] [--reps N] [--warmup N] [--latency]\n"
        "note: L0 uses --depth as its raw-thread parallelism (--workers is\n"
        "      ignored); L1/L2 use --workers as pool/backend worker count.\n"
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
        if (a == "--ladder") {
            std::string s(next("--ladder"));
            if (s == "L0") cfg.ladder = Ladder::L0_raw;
            else if (s == "L1") cfg.ladder = Ladder::L1_pool;
            else if (s == "L2") cfg.ladder = Ladder::L2_sluice;
            else return usage_error(argv[0], "--ladder must be L0|L1|L2");
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
        } else if (a == "--workers") {
            size_opt("--workers", cfg.workers);
        } else if (a == "--reps") {
            size_opt("--reps", cfg.reps);
        } else if (a == "--warmup") {
            size_opt("--warmup", cfg.warmup);
        } else if (a == "--latency") {
            cfg.latency = true;
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
    if (cfg.latency && rs.ops > (32u << 20))
        return usage_error(argv[0],
                           "--latency per-op array exceeds 32M entries");

    prepare_file(cfg, rs);

    int open_flags = cfg.op == Op::read ? O_RDONLY : O_WRONLY;
    rs.fd = ::open(cfg.file.c_str(), open_flags);
    if (rs.fd < 0) bench_fatal("open data file", errno);
    rs.init_buffers();
    if (cfg.ladder == Ladder::L2_sluice)
        rs.l2_comp = std::vector<sluice::async::Completion<std::size_t>>(
            cfg.depth);

    std::vector<RepResult> results;
    std::uint64_t setup_ns = 0, teardown_ns = 0;

    if (cfg.ladder == Ladder::L0_raw) {
        if (cfg.depth == 1) {
            for (std::size_t r = 0; r < cfg.warmup + cfg.reps; ++r) {
                RepResult res = run_l0_inline(rs);
                if (r >= cfg.warmup) {
                    rs.snapshot_latency(res);
                    results.push_back(res);
                }
            }
        } else {
            L0Threads pool;
            pool.rs = &rs;
            std::uint64_t s0 = now_ns();
            for (std::size_t t = 0; t < cfg.depth; ++t)
                pool.threads.emplace_back([&pool, t] { pool.worker(t); });
            setup_ns = now_ns() - s0;
            for (std::size_t r = 0; r < cfg.warmup + cfg.reps; ++r) {
                RepResult res = run_l0_parallel_rep(pool);
                if (r >= cfg.warmup) {
                    rs.snapshot_latency(res);
                    results.push_back(res);
                }
            }
            std::uint64_t s1 = now_ns();
            {
                std::lock_guard<std::mutex> lk(pool.m);
                pool.stop = true;
            }
            pool.start_cv.notify_all();
            for (auto& th : pool.threads) th.join();
            teardown_ns = now_ns() - s1;
        }
    } else if (cfg.ladder == Ladder::L1_pool) {
        L1Pool pool;
        pool.rs = &rs;
        pool.ring.assign(cfg.depth, 0);
        pool.done_flag.assign(cfg.depth, false);
        std::uint64_t s0 = now_ns();
        for (std::size_t w = 0; w < cfg.workers; ++w)
            pool.threads.emplace_back([&pool] { pool.worker(); });
        setup_ns = now_ns() - s0;
        for (std::size_t r = 0; r < cfg.warmup + cfg.reps; ++r) {
            RepResult res = run_l1_rep(pool);
            if (r >= cfg.warmup) {
                rs.snapshot_latency(res);
                results.push_back(res);
            }
        }
        std::uint64_t s1 = now_ns();
        {
            std::lock_guard<std::mutex> lk(pool.m);
            pool.stop = true;
        }
        pool.work_cv.notify_all();
        for (auto& th : pool.threads) th.join();
        teardown_ns = now_ns() - s1;
    } else {
        using namespace sluice::async;
        L2Runtime l2;
        std::uint64_t s0 = now_ns();
        {
            ThreadPoolConfig tc;
            tc.request_capacity = cfg.depth;
            tc.worker_count = cfg.workers;
            RuntimeBuilder builder;
            builder.backend(std::make_unique<ThreadPoolBackend>(tc));
            builder.workers(1);  // scheduler workers are a DISTINCT resource;
                                 // the G0 pipeline is one task, matching the
                                 // apps' run_task_to_result(workers=1) shape.
            auto built = builder.build();
            if (!built.has_value())
                bench_semantic("L2 RuntimeBuilder::build rejected the config");
            l2.rt = std::move(built.value());
            auto started = l2.rt->start();
            if (!started.has_value())
                bench_semantic("L2 ApplicationRuntime::start failed");
        }
        l2.setup_ns = setup_ns = now_ns() - s0;
        for (std::size_t r = 0; r < cfg.warmup + cfg.reps; ++r) {
            RepResult res = run_l2_rep(l2, rs);
            if (r >= cfg.warmup) {
                rs.snapshot_latency(res);
                results.push_back(res);
            }
        }
        std::uint64_t s1 = now_ns();
        l2.rt->request_stop();
        auto drained = l2.rt->drain();
        auto joined = l2.rt->join();
        teardown_ns = now_ns() - s1;
        if (!drained.has_value() || !joined.has_value())
            bench_semantic("L2 runtime drain/join failed");
    }

    // Fail-closed accounting: every rep must have moved exactly the whole
    // workload with zero errors, and (read) the exact expected word sum.
    bool all_ok = true;
    for (auto& r : results) {
        if (r.errors != 0 || r.ops != rs.ops || r.bytes != cfg.total_bytes ||
            (cfg.op == Op::read && r.word_sum != rs.expected_sum))
            all_ok = false;
    }
    if (cfg.op == Op::write) verify_written_file(cfg, rs.expected_sum);

    std::string out;
    out += "{\n";
    out += "  \"bench\": \"e1_abstraction_tax_bench\",\n";
    out += "  \"bench_version\": 1,\n";
    out += "  \"ladder\": \"" + std::string(ladder_name(cfg.ladder)) + "\",\n";
    out += std::string("  \"op\": \"") +
           (cfg.op == Op::read ? "read" : "write") + "\",\n";
    out += "  \"request_size\": " + std::to_string(cfg.request_size) + ",\n";
    out += "  \"total_bytes\": " + std::to_string(cfg.total_bytes) + ",\n";
    out += "  \"depth\": " + std::to_string(cfg.depth) + ",\n";
    out += "  \"workers\": " + std::to_string(cfg.workers) + ",\n";
    out += "  \"ops\": " + std::to_string(rs.ops) + ",\n";
    out += "  \"warmup\": " + std::to_string(cfg.warmup) + ",\n";
    out += "  \"reps\": " + std::to_string(cfg.reps) + ",\n";
    out += "  \"latency\": " + std::string(cfg.latency ? "true" : "false") +
           ",\n";
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
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "    {\"wall_ns\": %llu, \"user_ns\": %llu, "
                      "\"sys_ns\": %llu, \"maxrss_kb\": %llu, \"ops\": %llu, "
                      "\"bytes\": %llu, \"word_sum\": %llu, \"errors\": %llu, "
                      "\"lat_p50_ns\": %.1f, \"lat_p95_ns\": %.1f, "
                      "\"lat_p99_ns\": %.1f}%s\n",
                      (unsigned long long)r.wall_ns,
                      (unsigned long long)r.user_ns,
                      (unsigned long long)r.sys_ns,
                      (unsigned long long)r.maxrss_kb,
                      (unsigned long long)r.ops,
                      (unsigned long long)r.bytes,
                      (unsigned long long)r.word_sum,
                      (unsigned long long)r.errors, r.lat_p50_ns,
                      r.lat_p95_ns, r.lat_p99_ns,
                      (i + 1 < results.size()) ? "," : "");
        out += buf;
    }
    out += "  ]";
    if (cfg.latency) {
        // Final-rep raw per-op samples (evidence artifacts keep per-rep
        // percentiles above; this preserves one full distribution).
        out += ",\n  \"latency_samples_final_rep_ns\": [";
        for (std::size_t i = 0; i < rs.lat.size(); ++i) {
            if (i) out += ',';
            out += std::to_string(rs.lat[i]);
        }
        out += "]";
    }
    out += ",\n  \"all_reps_ok\": " + std::string(all_ok ? "true" : "false") +
           "\n}\n";
    std::fputs(out.c_str(), stdout);

    if (!all_ok) {
        std::fprintf(stderr,
                     "e1_abstraction_tax_bench: repetition accounting failed "
                     "(ops/bytes/errors%s mismatch)\n",
                     cfg.op == Op::read ? "/word_sum" : "");
        ::close(rs.fd);
        return 3;
    }
    ::close(rs.fd);
    return 0;
}
