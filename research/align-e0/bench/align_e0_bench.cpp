// ALIGN-E0 microbench (#265 Phase 3 — I/O buffer alignment threshold ×
// size × depth × direction). Research instrument. Prefaulted steady-state
// reuse regime (per-I/O alignment effect, NOT allocation/first-touch).
//
// Backing is IDENTICAL across arms: one over-allocated owned block per
// process (posix_memalign(&base, 4096, N + 4096)). Only the EXPOSED pointer
// changes:
//
//   --arm a0   exposed = base + 16   (natural production-like: 16-aligned,
//              page offset 16; glibc arena chunk-user-pointer shape)
//   --arm a1   exposed = round_up(base, 64)
//   --arm a2   exposed = round_up(base, 128)
//   --arm a3   exposed = round_up(base, 256)
//   --arm a4   exposed = round_up(base, 512)
//   --arm a5   exposed = round_up(base, 1024)
//   --arm a6   exposed = round_up(base, 2048)
//   --arm a7   exposed = round_up(base, 4096)
//   --offset X (PAGE-OFFSET-E0, arm ignored): exposed = base + X
//
// Directions (independent verdicts):
//   --dir read   pread(fd, exposed, N, off)   kernel -> userspace
//   --dir write  pwrite(fd, exposed, N, off)  userspace -> kernel
//
// Depth: --depth N = back-to-back batch of N syscalls per rep window
// (synchronous mode, workers=1 primary). --mode threaded (SECONDARY
// TOPOLOGY DIAGNOSTIC): --workers W threads, each with its own buffer at
// the same exposed alignment, barrier-synchronized; real in-flight depth =
// W. The threaded cell's op/offset pattern is identical to the sync cell
// with depth=W, so the overlap comparison is direct.
//
// Same-work contract (fail-closed, exit 3): every op returns exactly N
// bytes; READ in-loop mixed 64-bit word sum == expected; READ full FNV-1a
// of first/last rep OUTSIDE timed spans; WRITE source buffer full FNV
// verified OUTSIDE timed spans (source content byte-identical across arms);
// WRITE target file hashed by the runner post-exit. Offsets are the
// BUF-E0-line rotating chunk pattern within the cell's window.
//
// Workload bytes: TAX-0-line generator (4 KiB splitmix64 master block,
// kSeed 0xE1E1E1E121212121), identical across arms and sessions. Both
// --file (READ source) and --wfile (WRITE target) are 256 MiB deterministic
// tilings generated once per session environment by --generate.
//
// Warm page cache primary regime: untimed warm sweep over the cell's
// offsets before formal reps (same bytes as the measured run; for WRITE the
// warm sweep also writes the deterministic bytes, keeping the target file
// deterministic across arms).
//
// Output: one JSON object on stdout (consumed by
// research/align-e0/scripts/aligne0.py). Wall spans are steady_clock;
// fault deltas are getrusage(RUSAGE_SELF) around each timed region; perf
// counters are process-level and attributed by the runner via R7/R14
// double-difference.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

[[noreturn]] void bench_fatal(const char* what, int err) {
    std::fprintf(stderr, "align_e0_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(2);
}

[[noreturn]] void bench_semantic(const char* what) {
    std::fprintf(stderr, "align_e0_bench: semantic failure: %s\n", what);
    std::exit(3);
}

constexpr std::size_t kBlock = 4096;
constexpr std::uint64_t kSeed = 0xE1E1E1E121212121ull;

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

using MasterBlock = std::vector<std::byte>;

void fill_master_block(std::byte* master) {
    auto* w = reinterpret_cast<std::uint64_t*>(master);
    for (std::size_t i = 0; i < kBlock / sizeof(std::uint64_t); ++i)
        w[i] = splitmix64(kSeed + i);
}

// Expected content at any file offset: the master block tiled (the file is
// exactly the master block repeated).
std::uint64_t expected_word_sum(const std::byte* master, std::uint64_t off,
                                std::size_t len) {
    const auto* mw = reinterpret_cast<const std::uint64_t*>(master);
    std::uint64_t s = 0;
    std::size_t done = 0;
    std::size_t m = static_cast<std::size_t>(off % kBlock);
    while (done < len) {
        std::size_t chunk = std::min(len - done, kBlock - m);
        for (std::size_t i = 0; i < chunk; i += sizeof(std::uint64_t)) {
            // chunk and master-block offsets are 8-aligned by construction
            s += mw[(m + i) / sizeof(std::uint64_t)];
        }
        s = splitmix64(s);  // mix per block-segment: no trivial collisions
        done += chunk;
        m = (m + chunk) % kBlock;
    }
    return s;
}

std::uint64_t buffer_word_sum(const std::byte* p, std::size_t len) {
    auto* w = reinterpret_cast<const std::uint64_t*>(p);
    std::uint64_t s = 0;
    std::size_t done = 0;
    while (done < len) {
        std::size_t chunk = std::min(len - done, kBlock);
        for (std::size_t i = 0; i < chunk; i += sizeof(std::uint64_t))
            s += w[done / sizeof(std::uint64_t) + i / sizeof(std::uint64_t)];
        s = splitmix64(s);
        done += chunk;
    }
    return s;
}

std::uint64_t fnv1a(const std::byte* p, std::size_t len) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint8_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

std::uint64_t expected_fnv1a(const std::byte* master, std::uint64_t off,
                             std::size_t len) {
    std::uint64_t h = 1469598103934665603ull;
    std::size_t m = static_cast<std::size_t>(off % kBlock);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint8_t>(master[(m + i) % kBlock]);
        h *= 1099511628211ull;
    }
    return h;
}

// Deterministic source bytes for a WRITE buffer: the master tiling starting
// at page-aligned offset 0 of the buffer (byte-identical across arms).
void fill_source_buffer(std::byte* dst, const std::byte* master,
                        std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        std::size_t chunk = std::min(len - done, kBlock);
        std::memcpy(dst + done, master, chunk);
        done += chunk;
    }
}

// ---------------------------------------------------------------------------
// Measurement helpers
// ---------------------------------------------------------------------------

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct RUsage {
    std::int64_t minflt = 0;
    std::int64_t majflt = 0;
};

RUsage rusage_now() {
    rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) != 0) bench_fatal("getrusage", errno);
    return {ru.ru_minflt, ru.ru_majflt};
}

// ---------------------------------------------------------------------------
// Exposed buffer: one over-allocated owned block, exposed pointer selected
// per arm/offset. Backing mechanism is identical for every arm.
// ---------------------------------------------------------------------------

enum class Arm { a0, a1, a2, a3, a4, a5, a6, a7 };
enum class Mode { sync, threaded };

const char* arm_name(Arm a) {
    switch (a) {
    case Arm::a0: return "a0";
    case Arm::a1: return "a1";
    case Arm::a2: return "a2";
    case Arm::a3: return "a3";
    case Arm::a4: return "a4";
    case Arm::a5: return "a5";
    case Arm::a6: return "a6";
    case Arm::a7: return "a7";
    }
    return "?";
}

std::size_t arm_alignment(Arm a) {
    switch (a) {
    case Arm::a0: return 16;   // base+16 -> 16-byte aligned
    case Arm::a1: return 64;
    case Arm::a2: return 128;
    case Arm::a3: return 256;
    case Arm::a4: return 512;
    case Arm::a5: return 1024;
    case Arm::a6: return 2048;
    case Arm::a7: return 4096;
    }
    return 0;
}

// Round base up to the next multiple of `alignment` (power of two).
std::uintptr_t round_up(std::uintptr_t v, std::uintptr_t alignment) {
    return (v + alignment - 1) & ~(alignment - 1);
}

struct ExposedBuffer {
    void* base = nullptr;     // owning allocation (freed by free(base))
    std::byte* exposed = nullptr;
    std::size_t size = 0;     // usable bytes (N)
};

// One over-allocated owned block (N + 4096), page-aligned base; exposed =
// base + 16 (a0) or round_up(base, alignment) (a1..a7) or base + offset
// (PAGE-OFFSET-E0). Guard: exposed + N <= base + N + 4096 (offset <= 2048
// and alignment <= 4096 with page-aligned base both satisfy this).
ExposedBuffer make_buffer(Arm arm, bool offset_mode, std::size_t offset,
                          std::size_t n) {
    ExposedBuffer b;
    void* base = nullptr;
    if (::posix_memalign(&base, 4096, n + 4096) != 0)
        bench_fatal("posix_memalign", errno);
    b.base = base;
    b.size = n;
    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(base);
    if (offset_mode) {
        b.exposed = reinterpret_cast<std::byte*>(raw + offset);
    } else if (arm == Arm::a0) {
        b.exposed = reinterpret_cast<std::byte*>(raw + 16);
    } else {
        b.exposed = reinterpret_cast<std::byte*>(
            round_up(raw, arm_alignment(arm)));
    }
    if (reinterpret_cast<std::uintptr_t>(b.exposed) + n >
        raw + n + 4096)
        bench_semantic("exposed buffer exceeds owned block");
    return b;
}

void destroy_buffer(ExposedBuffer& b) {
    ::free(b.base);
    b.base = nullptr;
    b.exposed = nullptr;
}

// Identical prefault protocol for all arms: one byte write per 4096-byte
// page across the exposed buffer.
void prefault_buffer(std::byte* p, std::size_t n, std::size_t page) {
    for (std::size_t off = 0; off < n; off += page)
        p[off] = static_cast<std::byte>(0x5A);
}

// ---------------------------------------------------------------------------
// Raw positional I/O (measured path is plain pread/pwrite — the buffer
// geometry is the object, not any backend).
// ---------------------------------------------------------------------------

std::size_t pread_full(int fd, std::byte* dst, std::size_t len,
                       std::uint64_t off) {
    std::size_t done = 0;
    while (done < len) {
        ssize_t n = ::pread(fd, dst + done, len - done,
                            static_cast<off_t>(off + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            bench_fatal("pread", errno);
        }
        if (n == 0) bench_semantic("pread hit EOF inside the working window");
        done += static_cast<std::size_t>(n);
    }
    return done;
}

std::size_t pwrite_full(int fd, const std::byte* src, std::size_t len,
                        std::uint64_t off) {
    std::size_t done = 0;
    while (done < len) {
        ssize_t n = ::pwrite(fd, src + done, len - done,
                             static_cast<off_t>(off + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            bench_fatal("pwrite", errno);
        }
        if (n == 0) bench_semantic("pwrite made zero progress (backend fail)");
        done += static_cast<std::size_t>(n);
    }
    return done;
}

// ---------------------------------------------------------------------------
// Offset pattern — BUF-E0-line rotating chunks within the cell's window.
// ---------------------------------------------------------------------------

std::size_t clamp_kc(std::size_t n, std::size_t inflight) {
    // kc = clamp(256MiB / (inflight * size), 1, 16)
    const std::uint64_t window = 256ull << 20;
    std::uint64_t denom = static_cast<std::uint64_t>(inflight) * n;
    std::uint64_t kc = denom ? window / denom : 1;
    return static_cast<std::size_t>(std::clamp<std::uint64_t>(kc, 1, 16));
}

std::uint64_t chunk_offset(std::size_t slot, std::size_t cycle,
                           std::size_t inflight, std::size_t kc,
                           std::size_t n) {
    std::size_t idx = (slot + cycle * inflight) % (inflight * kc);
    return static_cast<std::uint64_t>(idx) * n;
}

// ---------------------------------------------------------------------------
// Per-rep records
// ---------------------------------------------------------------------------

struct RepSpan {
    std::uint64_t io_ns = 0;      // timed sweep (sync: whole rep; threaded:
                                  // rep wall = max over threads)
    std::uint64_t thread_wall_ns = 0;  // threaded: max thread op span
    std::int64_t minflt_io = 0;
    std::int64_t majflt = 0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    bool ok = true;
};

struct RunStats {
    std::vector<RepSpan> reps;
    std::uint64_t prefault_ns = 0;
    std::int64_t prefault_minflt = 0;
    std::uint64_t window_bytes = 0;
    std::size_t kc = 0;
    std::uint64_t strong_checks = 0;
    // threaded mode: per-thread per-op latencies (true per-op latency under
    // contention), aggregated across reps
    std::vector<std::uint64_t> thread_op_ns;
};

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct Config {
    bool write = false;            // false = read
    Arm arm = Arm::a0;
    bool offset_mode = false;
    std::size_t offset = 0;
    Mode mode = Mode::sync;
    std::size_t size = 4096;
    std::size_t depth = 1;
    std::size_t workers = 1;
    std::size_t reps = 7;
    std::string file, wfile;
    bool generate = false;
    std::uint64_t generate_bytes = 256ull << 20;
    std::string label;
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "align_e0_bench: missing value for %s\n",
                             what);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--dir") {
            std::string p = next("--dir");
            if (p == "read") c.write = false;
            else if (p == "write") c.write = true;
            else { std::fprintf(stderr, "bad --dir\n"); std::exit(1); }
        } else if (a == "--arm") {
            std::string p = next("--arm");
            if (p == "a0") c.arm = Arm::a0;
            else if (p == "a1") c.arm = Arm::a1;
            else if (p == "a2") c.arm = Arm::a2;
            else if (p == "a3") c.arm = Arm::a3;
            else if (p == "a4") c.arm = Arm::a4;
            else if (p == "a5") c.arm = Arm::a5;
            else if (p == "a6") c.arm = Arm::a6;
            else if (p == "a7") c.arm = Arm::a7;
            else { std::fprintf(stderr, "bad --arm\n"); std::exit(1); }
        } else if (a == "--offset") {
            c.offset_mode = true;
            c.offset = std::strtoull(next("--offset").c_str(), nullptr, 10);
        } else if (a == "--mode") {
            std::string p = next("--mode");
            if (p == "sync") c.mode = Mode::sync;
            else if (p == "threaded") c.mode = Mode::threaded;
            else { std::fprintf(stderr, "bad --mode\n"); std::exit(1); }
        } else if (a == "--size") {
            c.size = std::strtoull(next("--size").c_str(), nullptr, 10);
        } else if (a == "--depth") {
            c.depth = std::strtoull(next("--depth").c_str(), nullptr, 10);
        } else if (a == "--workers") {
            c.workers = std::strtoull(next("--workers").c_str(), nullptr, 10);
        } else if (a == "--reps") {
            c.reps = std::strtoull(next("--reps").c_str(), nullptr, 10);
        } else if (a == "--file") {
            c.file = next("--file");
        } else if (a == "--wfile") {
            c.wfile = next("--wfile");
        } else if (a == "--label") {
            c.label = next("--label");
        } else if (a == "--generate") {
            c.generate = true;
        } else if (a == "--generate-bytes") {
            c.generate_bytes =
                std::strtoull(next("--generate-bytes").c_str(), nullptr, 10);
        } else {
            std::fprintf(stderr, "align_e0_bench: unknown arg %s\n",
                         a.c_str());
            std::exit(1);
        }
    }
    return c;
}

// ---------------------------------------------------------------------------
// File generation (once per session environment, by the runner)
// ---------------------------------------------------------------------------

void generate_file(const std::string& path, const std::byte* master,
                   std::uint64_t bytes) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) bench_fatal("open(generate)", errno);
    std::vector<std::byte> chunk(1u << 20);
    for (std::size_t off = 0; off < chunk.size(); off += kBlock)
        std::memcpy(chunk.data() + off, master, kBlock);
    std::uint64_t written = 0;
    while (written < bytes) {
        std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(), bytes - written));
        ssize_t w = ::write(fd, chunk.data(), n);
        if (w < 0) {
            if (errno == EINTR) continue;
            bench_fatal("write(generate)", errno);
        }
        written += static_cast<std::uint64_t>(w);
    }
    if (::close(fd) != 0) bench_fatal("close(generate)", errno);
}

// ---------------------------------------------------------------------------
// Steady-state driver
// ---------------------------------------------------------------------------

// One op at `off` on the exposed buffer; verifies per direction. Returns
// the op span in ns.
std::uint64_t do_op(const Config& cfg, int fd, std::byte* exposed,
                    std::size_t n, std::uint64_t off,
                    const std::byte* master, bool* ok) {
    std::uint64_t t0 = now_ns();
    if (!cfg.write) {
        pread_full(fd, exposed, n, off);
        if (buffer_word_sum(exposed, n) !=
            expected_word_sum(master, off, n)) {
            *ok = false;
            bench_semantic("read word sum mismatch");
        }
    } else {
        pwrite_full(fd, exposed, n, off);
    }
    return now_ns() - t0;
}

struct ThreadCtx {
    const Config* cfg = nullptr;
    int fd = -1;
    const std::byte* master = nullptr;
    std::size_t worker = 0;
    std::size_t inflight = 0;    // workers (threaded) / depth (sync)
    std::size_t kc = 0;
    std::size_t n = 0;
    std::byte* exposed = nullptr;
    bool ok = true;
    std::uint64_t op_ns = 0;     // total op span for this thread (all reps)
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    std::vector<std::uint64_t> op_samples;  // per-op latencies
};

void* thread_main(void* arg) {
    // One steady-state pass = one rep (kc ops). The caller creates one
    // thread set per rep, so the thread body must NOT loop over reps.
    auto* tc = static_cast<ThreadCtx*>(arg);
    const Config& cfg = *tc->cfg;
    for (std::size_t c = 0; c < tc->kc; ++c) {
        std::uint64_t off =
            chunk_offset(tc->worker, c, tc->inflight, tc->kc, tc->n);
        std::uint64_t span = do_op(cfg, tc->fd, tc->exposed, tc->n, off,
                                   tc->master, &tc->ok);
        tc->op_ns += span;
        tc->op_samples.push_back(span);
        tc->ops++;
        tc->bytes += tc->n;
    }
    return nullptr;
}

int run_steady_state(const Config& cfg, int fd, const std::byte* master,
                     RunStats& rs) {
    const std::size_t n = cfg.size;
    const std::size_t inflight =
        cfg.mode == Mode::threaded ? cfg.workers : cfg.depth;
    const std::size_t page = 4096;

    rs.kc = clamp_kc(n, inflight);
    rs.window_bytes = static_cast<std::uint64_t>(inflight) * rs.kc * n;

    // Per-process buffer (sync) or per-thread buffers (threaded). Same
    // backing mechanism, same exposed alignment for every buffer.
    std::vector<ExposedBuffer> bufs(inflight);
    for (std::size_t i = 0; i < inflight; ++i)
        bufs[i] = make_buffer(cfg.arm, cfg.offset_mode, cfg.offset, n);

    // Identical prefault protocol before the timed region (same residency
    // for every arm: the backing page set is identical).
    std::uint64_t t0 = now_ns();
    RUsage p0 = rusage_now();
    for (auto& b : bufs) prefault_buffer(b.exposed, n, page);
    rs.prefault_ns = now_ns() - t0;
    RUsage p1 = rusage_now();
    rs.prefault_minflt = p1.minflt - p0.minflt;

    // Warm the page cache for the cell's offsets before formal reps
    // (untimed; same bytes as the measured run).
    {
        auto warm = make_buffer(cfg.arm, cfg.offset_mode, cfg.offset, n);
        for (std::size_t c = 0; c < rs.kc; ++c)
            for (std::size_t i = 0; i < inflight; ++i) {
                std::uint64_t off =
                    chunk_offset(i, c, inflight, rs.kc, n);
                if (!cfg.write) {
                    pread_full(fd, warm.exposed, n, off);
                } else {
                    fill_source_buffer(warm.exposed, master, n);
                    pwrite_full(fd, warm.exposed, n, off);
                }
            }
        destroy_buffer(warm);
    }

    if (cfg.mode == Mode::sync) {
        ExposedBuffer& b = bufs[0];
        if (!cfg.write) {
            // Source buffer is overwritten by reads; nothing to fill.
            (void)0;
        } else {
            // WRITE source content must be byte-identical across arms:
            // deterministic master tiling. Fill once before the timed
            // region (prefault already touched it; fill is deterministic).
            fill_source_buffer(b.exposed, master, n);
            if (fnv1a(b.exposed, n) != expected_fnv1a(master, 0, n))
                bench_semantic("write source fill mismatch");
            rs.strong_checks++;
        }
        for (std::size_t rep = 0; rep < cfg.reps; ++rep) {
            RepSpan r;
            RUsage a = rusage_now();
            std::uint64_t t1 = now_ns();
            for (std::size_t c = 0; c < rs.kc; ++c) {
                std::uint64_t off = chunk_offset(0, c, inflight, rs.kc, n);
                do_op(cfg, fd, b.exposed, n, off, master, &r.ok);
                r.ops++;
                r.bytes += n;
            }
            r.io_ns = now_ns() - t1;
            RUsage bb = rusage_now();
            r.minflt_io = bb.minflt - a.minflt;
            r.majflt = bb.majflt - a.majflt;
            // Strong verification outside the timed region.
            if (rep == 0 || rep + 1 == cfg.reps) {
                if (!cfg.write) {
                    std::uint64_t off =
                        chunk_offset(0, rep == 0 ? 0 : rs.kc - 1, inflight,
                                     rs.kc, n);
                    if (fnv1a(b.exposed, n) !=
                        expected_fnv1a(master, off, n))
                        bench_semantic("read FNV mismatch");
                    rs.strong_checks++;
                } else {
                    if (fnv1a(b.exposed, n) != expected_fnv1a(master, 0, n))
                        bench_semantic("write source FNV mismatch");
                    rs.strong_checks++;
                }
            }
            rs.reps.push_back(r);
        }
    } else {
        // Threaded (SECONDARY TOPOLOGY DIAGNOSTIC): workers threads, each
        // with its own buffer at the same exposed alignment, per-rep
        // create/join so every rep is a full steady-state pass over the
        // window (real in-flight depth = workers). Rep wall = the span from
        // the last thread creation to all joins (overlap view). Per-op
        // latencies are sampled per thread and aggregated across all reps.
        std::vector<pthread_t> tids(inflight);
        std::vector<ThreadCtx> ctxs(inflight);
        std::vector<std::uint64_t> rep_walls;
        rep_walls.reserve(cfg.reps);
        for (std::size_t w = 0; w < inflight; ++w) {
            auto& tc = ctxs[w];
            tc.cfg = &cfg;
            tc.fd = fd;
            tc.master = master;
            tc.worker = w;
            tc.inflight = inflight;
            tc.kc = rs.kc;
            tc.n = n;
            tc.exposed = bufs[w].exposed;
            tc.ok = true;
            if (cfg.write) {
                fill_source_buffer(tc.exposed, master, n);
                if (fnv1a(tc.exposed, n) != expected_fnv1a(master, 0, n))
                    bench_semantic("write source fill mismatch");
                rs.strong_checks++;
            }
        }
        for (std::size_t rep = 0; rep < cfg.reps; ++rep) {
            for (std::size_t w = 0; w < inflight; ++w) {
                auto& tc = ctxs[w];
                tc.op_ns = 0;
                tc.ops = 0;
                tc.bytes = 0;
                if (::pthread_create(&tids[w], nullptr, thread_main, &ctxs[w])
                    != 0)
                    bench_fatal("pthread_create", errno);
            }
            // Barrier: wait for all threads to finish their kc ops.
            std::uint64_t rep_t0 = now_ns();
            for (std::size_t w = 0; w < inflight; ++w)
                if (::pthread_join(tids[w], nullptr) != 0)
                    bench_fatal("pthread_join", errno);
            std::uint64_t rep_wall = now_ns() - rep_t0;
            rep_walls.push_back(rep_wall);

            RepSpan r;
            r.io_ns = rep_wall;
            for (std::size_t w = 0; w < inflight; ++w) {
                r.ops += ctxs[w].ops;
                r.bytes += ctxs[w].bytes;
                r.ok = r.ok && ctxs[w].ok;
                for (std::uint64_t s : ctxs[w].op_samples)
                    rs.thread_op_ns.push_back(s);
                ctxs[w].op_samples.clear();
            }
            // Strong verification outside the timed region (first/last rep):
            // the thread buffers hold the deterministic content (READ) or
            // source bytes (WRITE).
            if (rep == 0 || rep + 1 == cfg.reps) {
                for (std::size_t w = 0; w < inflight; ++w) {
                    std::uint64_t off =
                        chunk_offset(w, rep == 0 ? 0 : rs.kc - 1, inflight,
                                     rs.kc, n);
                    if (!cfg.write) {
                        if (fnv1a(bufs[w].exposed, n) !=
                            expected_fnv1a(master, off, n))
                            bench_semantic("threaded read FNV mismatch");
                    } else {
                        if (fnv1a(bufs[w].exposed, n) !=
                            expected_fnv1a(master, 0, n))
                            bench_semantic("threaded write source FNV mismatch");
                    }
                    rs.strong_checks++;
                }
            }
            rs.reps.push_back(r);
        }
    }

    for (auto& b : bufs) destroy_buffer(b);
    return 0;
}

// ---------------------------------------------------------------------------
// Stats helpers
// ---------------------------------------------------------------------------

std::uint64_t median_of(std::vector<std::uint64_t> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::uint64_t mad_of(const std::vector<std::uint64_t>& v,
                     std::uint64_t med) {
    if (v.empty()) return 0;
    std::vector<std::uint64_t> d;
    d.reserve(v.size());
    for (std::uint64_t x : v)
        d.push_back(x > med ? x - med : med - x);
    return median_of(std::move(d));
}

void json_escape(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                out += b;
            } else {
                out += c;
            }
        }
    }
}

std::string stat_line(const char* key, std::vector<std::uint64_t> v) {
    std::uint64_t m = median_of(v);
    std::uint64_t d = mad_of(v, m);
    std::uint64_t mn = v.empty() ? 0 : *std::min_element(v.begin(), v.end());
    std::uint64_t mx = v.empty() ? 0 : *std::max_element(v.begin(), v.end());
    char b[256];
    std::snprintf(b, sizeof(b),
                  "  \"%s\": {\"median\": %llu, \"mad\": %llu, "
                  "\"min\": %llu, \"max\": %llu, \"n\": %llu}",
                  key, (unsigned long long)m, (unsigned long long)d,
                  (unsigned long long)mn, (unsigned long long)mx,
                  (unsigned long long)v.size());
    return std::string(b);
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    if (cfg.size == 0 || cfg.reps == 0 || cfg.file.empty())
        std::exit(1);
    if (cfg.size % 4096 != 0)
        bench_semantic("--size must be page-multiple (prereg matrix)");
    if (cfg.offset_mode && cfg.offset > 2048)
        bench_semantic("--offset must be <= 2048 (owned-block guard)");
    if (cfg.mode == Mode::threaded && cfg.workers == 0)
        bench_semantic("--workers must be >= 1 in threaded mode");
    if (!cfg.write && cfg.wfile.empty())
        (void)0;  // read needs only --file; write needs --wfile
    if (cfg.write && cfg.wfile.empty())
        bench_semantic("--wfile required for --dir write");

    MasterBlock master(kBlock);
    fill_master_block(master.data());

    if (cfg.generate) {
        generate_file(cfg.file, master.data(), cfg.generate_bytes);
        if (!cfg.wfile.empty())
            generate_file(cfg.wfile, master.data(), cfg.generate_bytes);
        std::printf("generated %s and %s (%llu bytes each)\n",
                    cfg.file.c_str(), cfg.wfile.c_str(),
                    (unsigned long long)cfg.generate_bytes);
        return 0;
    }

    int fd = ::open(cfg.file.c_str(),
                    cfg.write ? O_WRONLY : O_RDONLY);
    if (fd < 0) bench_fatal("open(file)", errno);
    struct stat st{};
    if (::fstat(fd, &st) != 0) bench_fatal("fstat", errno);
    const std::uint64_t file_bytes = static_cast<std::uint64_t>(st.st_size);

    const std::size_t inflight =
        cfg.mode == Mode::threaded ? cfg.workers : cfg.depth;
    const std::size_t kc = clamp_kc(cfg.size, inflight);
    const std::uint64_t window = static_cast<std::uint64_t>(inflight) * kc *
                                 cfg.size;
    if (window > file_bytes)
        bench_semantic("working window exceeds data file");

    RunStats rs;
    int rc = run_steady_state(cfg, fd, master.data(), rs);
    ::close(fd);
    if (rc != 0) return rc;

    // ---- Aggregate + emit JSON ----
    std::vector<std::uint64_t> io_ns, minflt_io;
    std::uint64_t ops_total = 0, bytes_total = 0;
    bool all_ok = true;
    for (const auto& r : rs.reps) {
        io_ns.push_back(r.io_ns);
        minflt_io.push_back(static_cast<std::uint64_t>(
            r.minflt_io < 0 ? 0 : r.minflt_io));
        ops_total += r.ops;
        bytes_total += r.bytes;
        all_ok = all_ok && r.ok;
    }
    const std::size_t per_rep_ops = inflight * rs.kc;
    const std::size_t ops_for_diff = per_rep_ops * cfg.reps;

    std::string out = "{\n";
    out += "  \"bench\": \"align_e0_bench\",\n";
    {
        std::string esc;
        json_escape(esc, cfg.label);
        out += "  \"label\": \"" + esc + "\",\n";
    }
    out += std::string("  \"dir\": \"") + (cfg.write ? "write" : "read") +
           "\",\n";
    out += std::string("  \"mode\": \"") +
           (cfg.mode == Mode::threaded ? "threaded" : "sync") + "\",\n";
    if (cfg.offset_mode) {
        out += "  \"arm\": \"offset\",\n";
        out += "  \"offset\": " + std::to_string(cfg.offset) + ",\n";
        out += "  \"alignment\": " + std::to_string(cfg.offset) + ",\n";
        out += "  \"page_offset\": " +
               std::to_string(cfg.offset % 4096) + ",\n";
    } else {
        out += std::string("  \"arm\": \"") + arm_name(cfg.arm) + "\",\n";
        out += "  \"offset\": 0,\n";
        out += "  \"alignment\": " + std::to_string(arm_alignment(cfg.arm)) +
               ",\n";
        out += "  \"page_offset\": " +
               std::to_string(cfg.arm == Arm::a0 ? 16 : 0) + ",\n";
    }
    out += "  \"size\": " + std::to_string(cfg.size) + ",\n";
    out += "  \"depth\": " + std::to_string(cfg.depth) + ",\n";
    out += "  \"workers\": " + std::to_string(cfg.workers) + ",\n";
    out += "  \"reps\": " + std::to_string(cfg.reps) + ",\n";
    out += "  \"pages_per_buffer\": " + std::to_string(cfg.size / 4096) +
           ",\n";
    out += "  \"page_size\": 4096,\n";
    out += "  \"kc\": " + std::to_string(rs.kc) + ",\n";
    out += "  \"window_bytes\": " + std::to_string(rs.window_bytes) + ",\n";
    out += "  \"prefault_ns\": " + std::to_string(rs.prefault_ns) + ",\n";
    out += "  \"prefault_minflt\": " + std::to_string(rs.prefault_minflt) +
           ",\n";
    {
        std::string esc;
        json_escape(esc, cfg.file);
        out += "  \"file\": \"" + esc + "\",\n";
    }
    out += "  \"same_work\": {\"ops\": " + std::to_string(ops_total) +
           ", \"bytes\": " + std::to_string(bytes_total) +
           ", \"strong_checks\": " + std::to_string(rs.strong_checks) +
           ", \"ops_for_diff\": " + std::to_string(ops_for_diff) +
           ", \"ok\": " + (all_ok ? "true" : "false") + "},\n";
    out += stat_line("io_ns", io_ns) + ",\n";
    out += stat_line("minflt_io", minflt_io) + ",\n";
    if (cfg.mode == Mode::threaded && !rs.thread_op_ns.empty()) {
        out += stat_line("thread_op_ns", rs.thread_op_ns) + ",\n";
    }
    out += "  \"reps_detail\": [\n";
    for (std::size_t i = 0; i < rs.reps.size(); ++i) {
        const RepSpan& r = rs.reps[i];
        char b[320];
        std::snprintf(
            b, sizeof(b),
            "    {\"rep\": %llu, \"io_ns\": %llu, \"minflt_io\": %lld, "
            "\"majflt\": %lld, \"ops\": %llu, \"bytes\": %llu, "
            "\"ok\": %s}%s\n",
            (unsigned long long)i, (unsigned long long)r.io_ns,
            (long long)r.minflt_io, (long long)r.majflt,
            (unsigned long long)r.ops, (unsigned long long)r.bytes,
            r.ok ? "true" : "false",
            (i + 1 < rs.reps.size()) ? "," : "");
        out += b;
    }
    out += "  ],\n";
    out += "  \"all_reps_ok\": " + std::string(all_ok ? "true" : "false") +
           "\n}\n";
    std::fputs(out.c_str(), stdout);

    if (!all_ok) {
        std::fprintf(stderr, "align_e0_bench: same-work verification failed\n");
        return 3;
    }
    return 0;
}
