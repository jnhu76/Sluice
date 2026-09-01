// BUF-E0 microbench (#263 Phase 2 — buffer allocation / initialization /
// first-touch truth). Research instrument, research-only storage arms:
//
//   --arm b0  std::vector<std::byte>(N)              (production reference)
//   --arm b1  std::make_unique_for_overwrite<byte[]> (uninitialized owned)
//   --arm b2  anonymous mmap, no MAP_POPULATE        (demand paging)
//   --arm b3  posix_memalign(4096, N)                (page-aligned owned)
//
//   --phase A  allocation -> ready            (construction span per rep)
//   --phase B  allocation -> first useful I/O (pread into fresh buffer,
//              no manual prefault; alloc span + first-I/O span, separate)
//   --phase C  prefaulted steady-state reuse  (identical prefault protocol,
//              then sweeps of preads, no allocation in the timed region)
//   --phase D  memory-only first-touch diagnostic (one write per page)
//
// Fresh-page regime (phases A/B/D, --regime pinned, the default): mallopt
// pins M_MMAP_THRESHOLD=4096 so every B0/B1/B3 construction is a fresh mmap
// with never-touched pages and teardown munmaps — deterministic cold-start
// lifecycle per rep, identical allocator mechanism across arms. Phase C is
// unaffected (buffers held for the whole phase). --regime arena keeps glibc
// defaults (secondary exploratory probe only).
//
// Same-work contract (fail-closed, exit 3): every read returns exactly N
// bytes; in-loop cheap verification is a mixed 64-bit word sum that must
// equal the generator's expected sum at the same offset (identical across
// arms); Phase B additionally verifies FNV-1a OUTSIDE timed spans. Bytes,
// read counts, and verification results are reported per run.
//
// Workload bytes: IDENTICAL generator to the TAX-0 line (4 KiB splitmix64
// master block, kSeed 0xE1E1E1E121212121). The data file is generated once
// per session environment by --generate and only read by measured runs.
//
// Output: one JSON object on stdout (consumed by
// research/buf-e0/scripts/bufe0.py). Wall spans are steady_clock; fault
// deltas are getrusage(RUSAGE_SELF) around each timed region; resident
// deltas are /proc/self/statm snapshots. perf counters are process-level
// and attributed by the runner via R7/R14 double-difference.

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
#include <vector>

#include <fcntl.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

[[noreturn]] void bench_fatal(const char* what, int err) {
    std::fprintf(stderr, "buf_e0_bench: fatal: %s (errno=%d: %s)\n", what,
                 err, std::strerror(err));
    std::exit(2);
}

[[noreturn]] void bench_semantic(const char* what) {
    std::fprintf(stderr, "buf_e0_bench: semantic failure: %s\n", what);
    std::exit(3);
}

// ---------------------------------------------------------------------------
// Deterministic workload bytes — TAX-0-line generator (same kSeed, same
// splitmix64 4 KiB master block) so the campaign input pattern family stays
// byte-identical across benches.
// ---------------------------------------------------------------------------

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

// Expected content at any file offset: the master block tiled. The file is
// exactly the master block repeated, so expected bytes at `off` come from
// master[(off % 4096) ...] with wraparound.
// Cheap in-loop verification: mixed 64-bit word sum (identical across arms).
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
    // Must mirror expected_word_sum's per-4096 mixing exactly.
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
    std::int64_t maxrss_kb = 0;
};

RUsage rusage_now() {
    rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) != 0) bench_fatal("getrusage", errno);
    RUsage r;
    r.minflt = ru.ru_minflt;
    r.majflt = ru.ru_majflt;
    r.maxrss_kb = ru.ru_maxrss;
    return r;
}

// ---------------------------------------------------------------------------
// Raw-syscall /proc readers. MUST NOT use stdio or malloc: a freed stdio
// buffer lands in the malloc arena as a recyclable >=4 KiB chunk, and later
// constructions would come from the arena instead of fresh mmap, silently
// breaking the pinned fresh-page regime (found by instrument check).
// ---------------------------------------------------------------------------

std::int64_t resident_kb_now() {
    int fd = ::open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return -1;
    char buf[128];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    // fields: total resident shared text lib data dt
    unsigned long long total = 0, resident_pages = 0;
    char* p = buf;
    // first field
    total = std::strtoull(p, &p, 10);
    resident_pages = std::strtoull(p + 1, &p, 10);
    (void)total;
    return static_cast<std::int64_t>(resident_pages) * 4096 / 1024;
}

// Classify where a pointer lives, from /proc/self/maps via raw syscalls.
// Returns "brk" if inside the [heap] region, "anon-mmap" if inside any
// anonymous mapping, "?" otherwise.
void maps_kind(const void* ptr, char* out, std::size_t out_n) {
    out[0] = '\0';
    int fd = ::open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        std::snprintf(out, out_n, "?");
        return;
    }
    static char buf[1 << 16];  // static: no allocation in the measured path
    std::size_t have = 0;
    buf[0] = '\0';
    const auto* p = static_cast<const unsigned char*>(ptr);
    bool decided = false;
    while (!decided) {
        ssize_t n = ::read(fd, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0) break;
        have += static_cast<std::size_t>(n);
        buf[have] = '\0';
        // parse complete lines
        char* line = buf;
        while (char* nl = std::strchr(line, '\n')) {
            *nl = '\0';
            unsigned long a = 0, b = 0;
            if (std::sscanf(line, "%lx-%lx", &a, &b) == 2) {
                auto addr = reinterpret_cast<unsigned long>(p);
                if (addr >= a && addr < static_cast<unsigned long>(b)) {
                    bool heap = std::strstr(line, "[heap]") != nullptr;
                    std::snprintf(out, out_n, "%s",
                                  heap ? "brk" : "anon-mmap");
                    // named file mappings (libs) still report as anon-mmap;
                    // the arms' buffers are anonymous by construction
                    decided = true;
                    break;
                }
            }
            line = nl + 1;
        }
        if (decided) break;
        // keep the trailing partial line
        std::size_t consumed = static_cast<std::size_t>(line - buf);
        std::memmove(buf, line, have - consumed + 1);
        have -= consumed;
        if (have + 1 >= sizeof(buf)) break;
    }
    ::close(fd);
    if (!decided && out[0] == '\0') std::snprintf(out, out_n, "?");
}

// ---------------------------------------------------------------------------
// Storage arms — research-only. Each provides exactly N writable contiguous
// bytes; teardown dispatch is by arm. (BUF-E0-PREREGISTRATION §1.)
// ---------------------------------------------------------------------------

enum class Arm { b0, b1, b2, b3 };
enum class Phase { A, B, C, D };
enum class Regime { pinned, arena };

const char* arm_name(Arm a) {
    switch (a) {
    case Arm::b0: return "b0";
    case Arm::b1: return "b1";
    case Arm::b2: return "b2";
    case Arm::b3: return "b3";
    }
    return "?";
}

struct Buffer {
    std::byte* data = nullptr;
    std::size_t size = 0;
    // owning context, arm-specific
    std::vector<std::byte>* b0_vec = nullptr;              // b0
    std::byte* b1_ptr = nullptr;                           // b1
    void* b2_map = nullptr;                                // b2
    void* b3_ptr = nullptr;                                // b3
};

void buffer_construct(Arm arm, std::size_t n, Buffer& out) {
    out.size = n;
    switch (arm) {
    case Arm::b0: {
        auto* v = new std::vector<std::byte>(n);
        out.b0_vec = v;
        out.data = v->data();
        break;
    }
    case Arm::b1: {
        auto p = std::make_unique_for_overwrite<std::byte[]>(n);
        out.data = p.get();
        out.b1_ptr = p.release();
        break;
    }
    case Arm::b2: {
        void* m = ::mmap(nullptr, n, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) bench_fatal("mmap", errno);
        out.b2_map = m;
        out.data = static_cast<std::byte*>(m);
        break;
    }
    case Arm::b3: {
        void* p = nullptr;
        if (::posix_memalign(&p, 4096, n) != 0)
            bench_fatal("posix_memalign", errno);
        out.b3_ptr = p;
        out.data = static_cast<std::byte*>(p);
        break;
    }
    }
}

void buffer_destroy(Arm arm, Buffer& b) {
    switch (arm) {
    case Arm::b0:
        delete b.b0_vec;
        b.b0_vec = nullptr;
        break;
    case Arm::b1:
        delete[] b.b1_ptr;
        b.b1_ptr = nullptr;
        break;
    case Arm::b2:
        if (::munmap(b.b2_map, b.size) != 0) bench_fatal("munmap", errno);
        b.b2_map = nullptr;
        break;
    case Arm::b3:
        ::free(b.b3_ptr);
        b.b3_ptr = nullptr;
        break;
    }
    b.data = nullptr;
}

using Slots = std::vector<Buffer>;

void slots_construct(Arm arm, std::size_t n, std::size_t count, Slots& out) {
    out.assign(count, Buffer{});
    for (std::size_t i = 0; i < count; ++i)
        buffer_construct(arm, n, out[i]);
}

void slots_destroy(Arm arm, Slots& s) {
    for (auto& b : s)
        if (b.data) buffer_destroy(arm, b);
    s.clear();
}

// Identical prefault protocol for all arms (Phase C): one write per page.
void prefault_slots(Slots& s, std::size_t page) {
    for (auto& b : s)
        for (std::size_t off = 0; off < b.size; off += page)
            b.data[off] = static_cast<std::byte>(0x5A);
}

// Phase D deterministic first touch: one write per page (same protocol as
// the Phase C prefault; here it IS the measured operation).
void first_touch_slots(Slots& s, std::size_t page) {
    for (auto& b : s)
        for (std::size_t off = 0; off < b.size; off += page)
            b.data[off] = static_cast<std::byte>(0x5A);
}

// ---------------------------------------------------------------------------
// Raw positional read (measured path is plain pread — the buffer lifecycle
// is the object, not any backend).
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

// ---------------------------------------------------------------------------
// Rep-recorded spans and statistics
// ---------------------------------------------------------------------------

struct RepSpan {
    std::uint64_t alloc_ns = 0;
    std::uint64_t io_ns = 0;    // phase B first-I/O span; phase C sweep span
    std::uint64_t touch_ns = 0; // phase D
    std::uint64_t destroy_ns = 0;
    // Fault attribution PER REGION: the cost-shift question needs to know
    // WHERE faults landed (construct-time init vs first-I/O vs first touch),
    // not just how many there were.
    std::int64_t minflt_alloc = 0;
    std::int64_t minflt_io = 0;     // phase B first-I/O / phase C sweep
    std::int64_t minflt_touch = 0;  // phase D first touch
    std::int64_t majflt = 0;
    std::int64_t rss_before_kb = -1;
    std::int64_t rss_after_kb = -1;
    char slot0_kind[16] = "?";  // regime evidence: where slot 0's buffer lives
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    bool ok = true;
};

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

// Seed a small-chunk reservoir (pinned regime). Sub-threshold allocations
// (e.g. the 32-byte `new std::vector<std::byte>` object header) would
// otherwise sbrk-extend the exhausted top and re-enable brk carving for the
// measured buffers. The reservoir: 64 free 256-byte chunks, each bounded by
// a LIVE 16-byte guard allocation so consolidation can never merge them
// into a run large enough to serve a buffer or eater request (>=4112).
// Later small requests split from these chunks forever; the guards leak by
// design (process lifetime).
void seed_small_chunk_reservoir() {
    void* pieces[64];
    void* guards[64];
    (void)guards;
    for (int i = 0; i < 64; ++i) {
        pieces[i] = std::malloc(256);
        guards[i] = std::malloc(16);
        if (!pieces[i] || !guards[i]) return;
    }
    for (int i = 0; i < 64; ++i) std::free(pieces[i]);
    // guards leak by design: their allocations stay live for the process
    // lifetime, bounding the reservoir pieces; the pointer array itself is
    // stack-local and simply goes out of scope.
}

// Exhaust the malloc arena's brk top chunk (pinned regime). glibc carves
// small-enough requests from the top chunk BEFORE the sysmalloc mmap path
// is consulted, so a fresh arena's ~128 KiB top silently serves 4 KiB
// constructions from brk. Holding a chain of 4 KiB allocations until the
// maps check proves new ones come from anon-mmap pins the regime: every
// later >=threshold construction reaches sysmalloc and mmaps fresh pages.
// The eater chain is STATIC and never freed (freeing would undo the
// exhaustion; mmap'd frees also stay clean because they munmap, and the
// chain's pointer buffer is itself >=threshold and held).
void exhaust_arena_top() {
    static std::vector<void*> held;  // process-lifetime; never destroyed
    held.reserve(1088);
    char kind[16] = "?";
    int consecutive_mmap = 0;
    for (int i = 0; i < 1088 && consecutive_mmap < 8; ++i) {
        void* p = std::malloc(4096);
        if (!p) break;
        held.push_back(p);
        maps_kind(p, kind, sizeof(kind));
        if (std::strcmp(kind, "anon-mmap") == 0)
            ++consecutive_mmap;
        else
            consecutive_mmap = 0;
    }
    if (std::getenv("BUFE0_DEBUG"))
        std::fprintf(stderr, "bufe0 exhaust: eaters=%zu last_kind=%s\n",
                     held.size(), kind);
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct Config {
    Phase phase = Phase::A;
    Arm arm = Arm::b0;
    Regime regime = Regime::pinned;
    std::size_t size = 4096;
    std::size_t slots = 1;
    std::size_t reps = 7;
    std::size_t kc = 0;  // phase C reuse per slot; 0 = auto (prereg formula)
    std::string file;
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
                std::fprintf(stderr, "buf_e0_bench: missing value for %s\n",
                             what);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--phase") {
            std::string p = next("--phase");
            if (p == "A") c.phase = Phase::A;
            else if (p == "B") c.phase = Phase::B;
            else if (p == "C") c.phase = Phase::C;
            else if (p == "D") c.phase = Phase::D;
            else { std::fprintf(stderr, "bad --phase\n"); std::exit(1); }
        } else if (a == "--arm") {
            std::string p = next("--arm");
            if (p == "b0") c.arm = Arm::b0;
            else if (p == "b1") c.arm = Arm::b1;
            else if (p == "b2") c.arm = Arm::b2;
            else if (p == "b3") c.arm = Arm::b3;
            else { std::fprintf(stderr, "bad --arm\n"); std::exit(1); }
        } else if (a == "--regime") {
            std::string p = next("--regime");
            if (p == "pinned") c.regime = Regime::pinned;
            else if (p == "arena") c.regime = Regime::arena;
            else { std::fprintf(stderr, "bad --regime\n"); std::exit(1); }
        } else if (a == "--size") {
            c.size = std::strtoull(next("--size").c_str(), nullptr, 10);
        } else if (a == "--slots") {
            c.slots = std::strtoull(next("--slots").c_str(), nullptr, 10);
        } else if (a == "--reps") {
            c.reps = std::strtoull(next("--reps").c_str(), nullptr, 10);
        } else if (a == "--kc") {
            c.kc = std::strtoull(next("--kc").c_str(), nullptr, 10);
        } else if (a == "--file") {
            c.file = next("--file");
        } else if (a == "--label") {
            c.label = next("--label");
        } else if (a == "--generate") {
            c.generate = true;
        } else if (a == "--generate-bytes") {
            c.generate_bytes =
                std::strtoull(next("--generate-bytes").c_str(), nullptr, 10);
        } else {
            std::fprintf(stderr, "buf_e0_bench: unknown arg %s\n", a.c_str());
            std::exit(1);
        }
    }
    return c;
}

// ---------------------------------------------------------------------------
// File generation (once per session environment, by the runner)
// ---------------------------------------------------------------------------

void generate_file(const Config& cfg, const std::byte* master) {
    int fd = ::open(cfg.file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) bench_fatal("open(generate)", errno);
    std::vector<std::byte> chunk(1u << 20);
    for (std::size_t off = 0; off < chunk.size(); off += kBlock)
        std::memcpy(chunk.data() + off, master, kBlock);
    std::uint64_t written = 0;
    while (written < cfg.generate_bytes) {
        std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(),
                                    cfg.generate_bytes - written));
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
// Phase drivers. Every span's fault deltas come from getrusage around the
// timed region; RSS snapshots bracket the alloc region.
// ---------------------------------------------------------------------------

struct RunStats {
    std::vector<RepSpan> reps;
    std::uint64_t construct_ns = 0;   // phase C setup (untimed regime entry)
    std::int64_t construct_minflt = 0;
    char construct_kind[16] = "?";    // phase C: slot 0 mapping kind
    std::uint64_t prefault_ns = 0;
    std::int64_t prefault_minflt = 0;
    std::uint64_t window_bytes = 0;
    std::size_t kc = 0;
    std::uint64_t strong_checks = 0;
    bool all_ok = true;
};

// Per-process op count for the runner's R7/R14 double-difference
// normalization (prereg §6): A/D = slots*reps constructions;
// B = slots*reps first-reads; C = measured sweep reads.
std::uint64_t ops_for_diff(const Config& cfg, const RunStats& rs) {
    switch (cfg.phase) {
    case Phase::A:
    case Phase::D:
    case Phase::B:
        return static_cast<std::uint64_t>(cfg.slots) * cfg.reps;
    case Phase::C:
        return static_cast<std::uint64_t>(cfg.slots) * rs.kc * cfg.reps;
    }
    return 0;
}

std::uint64_t phase_b_offset(std::size_t slot, std::size_t n) {
    return static_cast<std::uint64_t>(slot) * n;
}

// Phase C chunk index: slot s at cycle c reads chunk (s + c*slots) within a
// window of slots*K chunks (each sweep covers the window exactly once).
std::uint64_t phase_c_offset(std::size_t slot, std::size_t cycle,
                             std::size_t slots, std::size_t kc,
                             std::size_t n) {
    std::size_t idx = (slot + cycle * slots) % (slots * kc);
    return static_cast<std::uint64_t>(idx) * n;
}

int run_phase(const Config& cfg, int fd, const std::byte* master,
              RunStats& rs) {
    const std::size_t n = cfg.size;
    const std::size_t slots = cfg.slots;
    const std::size_t page = 4096;

    // All non-measured allocations FIRST (reserve rep records so the vector
    // never grows inside the measured region — growth would sbrk-extend the
    // arena top and silently re-enable brk carving for the buffers).
    rs.reps.reserve(cfg.reps);

    if (cfg.phase == Phase::C) {
        rs.kc = cfg.kc != 0
                    ? cfg.kc
                    : std::clamp<std::size_t>((256ull << 20) / (slots * n),
                                              1, 16);
        rs.window_bytes = static_cast<std::uint64_t>(slots) * rs.kc * n;
    }

    Slots s;
    // Pre-establish the Slots vector's capacity BEFORE the regime pinning:
    // the first assign() would otherwise allocate the slot-record array
    // inside the measured region, and for small slot counts that sub-4112
    // allocation sbrk-refills the exhausted top (regime break).
    s.reserve(slots);
    std::vector<std::uint64_t> alloc_ns, io_ns, touch_ns, destroy_ns;

    // Warm the page cache for the cell's read offsets before formal reps
    // (phases B/C; untimed; intent: warm — prereg §4). Uses a single temp
    // buffer outside the measured slots.
    if (cfg.phase == Phase::B || cfg.phase == Phase::C) {
        auto warm = std::make_unique_for_overwrite<std::byte[]>(n);
        if (cfg.phase == Phase::B) {
            for (std::size_t i = 0; i < slots; ++i)
                pread_full(fd, warm.get(), n, phase_b_offset(i, n));
        } else {
            const std::size_t kc = rs.kc;
            for (std::size_t c = 0; c < kc; ++c)
                for (std::size_t i = 0; i < slots; ++i)
                    pread_full(fd, warm.get(), n,
                               phase_c_offset(i, c, slots, kc, n));
        }
    }

    // Fresh-page regime: exhaust the arena top chunk LAST — after every
    // setup allocation (reserves, warm buffer) — so no later small
    // allocation can sbrk-refill the top before the measured region ends.
    // Phase C's construct+prefault follows immediately; A/B/D's rep loop
    // contains no allocation by construction (reserved vectors, stable
    // Slots capacity, raw-syscall probes).
    if (cfg.regime == Regime::pinned) {
        seed_small_chunk_reservoir();
        exhaust_arena_top();
    }

    if (cfg.phase == Phase::C) {
        // One residency construction + identical prefault protocol, then the
        // measurement loop reuses the same buffers every sweep (production
        // reuse semantics; no allocation inside the timed region). The
        // construction and prefault spans are recorded separately so their
        // fault attribution does not mix.
        RUsage c0 = rusage_now();
        std::uint64_t tc = now_ns();
        slots_construct(cfg.arm, n, slots, s);
        rs.construct_ns = now_ns() - tc;
        RUsage c1 = rusage_now();
        rs.construct_minflt = c1.minflt - c0.minflt;
        if (cfg.arm != Arm::b2)
            maps_kind(s[0].data, rs.construct_kind,
                      sizeof(rs.construct_kind));
        else
            std::snprintf(rs.construct_kind, sizeof(rs.construct_kind),
                          "own-mmap");
        std::uint64_t t0 = now_ns();
        prefault_slots(s, page);
        rs.prefault_ns = now_ns() - t0;
        RUsage r1 = rusage_now();
        rs.prefault_minflt = r1.minflt - c1.minflt;
    }

    for (std::size_t rep = 0; rep < cfg.reps; ++rep) {
        RepSpan r;
        if (cfg.phase == Phase::C) {
            // Timed region: one sweep = kc cycles x slots reads, in-loop
            // mixed word-sum verification (identical across arms).
            const std::size_t kc = rs.kc;
            RUsage a = rusage_now();
            std::uint64_t t0 = now_ns();
            for (std::size_t c = 0; c < kc; ++c) {
                for (std::size_t i = 0; i < slots; ++i) {
                    std::uint64_t off = phase_c_offset(i, c, slots, kc, n);
                    pread_full(fd, s[i].data, n, off);
                    if (buffer_word_sum(s[i].data, n) !=
                        expected_word_sum(master, off, n)) {
                        r.ok = false;
                        bench_semantic("phase C word sum mismatch");
                    }
                    r.ops++;
                    r.bytes += n;
                }
            }
            r.io_ns = now_ns() - t0;
            RUsage b = rusage_now();
            r.minflt_io = b.minflt - a.minflt;
            r.majflt = b.majflt - a.majflt;
            // Strong verification outside the timed region: first and last
            // sweep FNV per slot.
            if (rep == 0 || rep + 1 == cfg.reps) {
                for (std::size_t i = 0; i < slots; ++i) {
                    std::uint64_t off =
                        phase_c_offset(i, rep == 0 ? 0 : rs.kc - 1, slots,
                                       rs.kc, n);
                    if (fnv1a(s[i].data, n) != expected_fnv1a(master, off, n))
                        bench_semantic("phase C FNV mismatch");
                    rs.strong_checks++;
                }
            }
            rs.reps.push_back(r);
            continue;
        }

        // Phases A / B / D: fresh construction per rep (pinned fresh-page
        // regime by default), phase-specific measured region, teardown.
        // Re-exhaust the arena top before every rep: glibc arena dynamics
        // (fastbin/tcache cycling of the many sub-threshold object headers)
        // can silently sbrk-refill the top between reps at higher slot
        // counts; the eaters are held, untouched (no faults), and outside
        // every timed span. The per-rep regime gate verifies the result.
        if (cfg.regime == Regime::pinned) exhaust_arena_top();
        RUsage ra = rusage_now();
        std::int64_t rss_before = resident_kb_now();
        std::uint64_t t0 = now_ns();
        slots_construct(cfg.arm, n, slots, s);
        std::uint64_t t1 = now_ns();
        r.alloc_ns = t1 - t0;
        std::int64_t rss_after = resident_kb_now();
        RUsage rb = rusage_now();
        r.minflt_alloc = rb.minflt - ra.minflt;
        r.majflt = rb.majflt - ra.majflt;
        r.rss_before_kb = rss_before;
        r.rss_after_kb = rss_after;
        // Regime evidence (fail-closed checked against the pinned regime in
        // the driver): where did slot 0's fresh buffer actually come from?
        if (cfg.arm != Arm::b2)
            maps_kind(s[0].data, r.slot0_kind, sizeof(r.slot0_kind));
        else
            std::snprintf(r.slot0_kind, sizeof(r.slot0_kind), "own-mmap");

        if (cfg.phase == Phase::B) {
            // First useful I/O per slot: real pread overwriting the fresh
            // buffer; verification (FNV, strong) OUTSIDE the timed span.
            RUsage ia = rusage_now();
            std::uint64_t t2 = now_ns();
            for (std::size_t i = 0; i < slots; ++i) {
                pread_full(fd, s[i].data, n, phase_b_offset(i, n));
                r.ops++;
                r.bytes += n;
            }
            r.io_ns = now_ns() - t2;
            RUsage ib = rusage_now();
            r.minflt_io = ib.minflt - ia.minflt;
            for (std::size_t i = 0; i < slots; ++i) {
                std::uint64_t off = phase_b_offset(i, n);
                if (fnv1a(s[i].data, n) != expected_fnv1a(master, off, n))
                    bench_semantic("phase B FNV mismatch");
                rs.strong_checks++;
            }
        } else if (cfg.phase == Phase::D) {
            // Memory-only deterministic first touch: one write per page.
            RUsage ta = rusage_now();
            std::uint64_t t2 = now_ns();
            first_touch_slots(s, page);
            r.touch_ns = now_ns() - t2;
            RUsage tb = rusage_now();
            r.minflt_touch = tb.minflt - ta.minflt;
        }

        std::uint64_t t3 = now_ns();
        slots_destroy(cfg.arm, s);
        r.destroy_ns = now_ns() - t3;
        r.ok = true;
        rs.reps.push_back(r);
    }

    if (cfg.phase == Phase::C) slots_destroy(cfg.arm, s);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    if (cfg.size == 0 || cfg.slots == 0 || cfg.reps == 0 || cfg.file.empty())
        std::exit(1);
    if (cfg.size % 4096 != 0)
        bench_semantic("--size must be page-multiple (prereg matrix)");
    // Memory guard (prereg §3)
    {
        std::uint64_t cell = static_cast<std::uint64_t>(cfg.size) * cfg.slots;
        if (cell > (512ull << 20)) {
            std::fprintf(stderr,
                         "buf_e0_bench: SKIPPED - MEMORY BUDGET "
                         "(cell=%llu bytes)\n",
                         (unsigned long long)cell);
            std::exit(4);
        }
    }

    // Fresh-page regime pinning BEFORE any buffer work (prereg §5): pin the
    // malloc threshold AND (inside run_phase, after all setup allocations)
    // exhaust the arena top chunk so every measured construction reaches
    // sysmalloc's mmap path (fresh, never-touched pages) instead of being
    // carved from brk.
    if (cfg.regime == Regime::pinned) {
        ::mallopt(M_MMAP_THRESHOLD, 4096);
        ::mallopt(M_TRIM_THRESHOLD, 128 * 1024);
        ::mallopt(M_ARENA_MAX, 1);
    }

    MasterBlock master(kBlock);
    fill_master_block(master.data());

    if (cfg.generate) {
        generate_file(cfg, master.data());
        std::printf("generated %s (%llu bytes)\n", cfg.file.c_str(),
                    (unsigned long long)cfg.generate_bytes);
        return 0;
    }

    int fd = ::open(cfg.file.c_str(), O_RDONLY);
    if (fd < 0) bench_fatal("open(file)", errno);
    struct stat st{};
    if (::fstat(fd, &st) != 0) bench_fatal("fstat", errno);
    const std::uint64_t file_bytes = static_cast<std::uint64_t>(st.st_size);

    // Working-window bounds guard: every planned offset+N must sit inside
    // the file (same-work: full reads only).
    {
        std::uint64_t need = 0;
        if (cfg.phase == Phase::B)
            need = static_cast<std::uint64_t>(cfg.slots) * cfg.size;
        else if (cfg.phase == Phase::C)
            need = static_cast<std::uint64_t>(cfg.slots) *
                   (cfg.kc != 0
                        ? cfg.kc
                        : std::clamp<std::size_t>((256ull << 20) /
                                                      (cfg.slots * cfg.size),
                                                  1, 16)) *
                   cfg.size;
        if (cfg.phase == Phase::B || cfg.phase == Phase::C) {
            if (need > file_bytes)
                bench_semantic("working window exceeds data file");
        }
    }

    RunStats rs;
    int rc = run_phase(cfg, fd, master.data(), rs);
    ::close(fd);
    if (rc != 0) return rc;

    // ---- Aggregate + emit JSON ----
    std::vector<std::uint64_t> alloc_ns, io_ns, touch_ns, destroy_ns;
    std::vector<std::uint64_t> minflt_alloc, minflt_io, minflt_touch;
    std::uint64_t ops_total = 0, bytes_total = 0;
    bool all_ok = true;
    for (const auto& r : rs.reps) {
        alloc_ns.push_back(r.alloc_ns);
        io_ns.push_back(r.io_ns);
        touch_ns.push_back(r.touch_ns);
        destroy_ns.push_back(r.destroy_ns);
        minflt_alloc.push_back(static_cast<std::uint64_t>(
            r.minflt_alloc < 0 ? 0 : r.minflt_alloc));
        minflt_io.push_back(static_cast<std::uint64_t>(
            r.minflt_io < 0 ? 0 : r.minflt_io));
        minflt_touch.push_back(static_cast<std::uint64_t>(
            r.minflt_touch < 0 ? 0 : r.minflt_touch));
        ops_total += r.ops;
        bytes_total += r.bytes;
        all_ok = all_ok && r.ok;
    }
    auto med = [](std::vector<std::uint64_t> v) { return median_of(v); };
    auto stat_line = [&](const char* key, std::vector<std::uint64_t> v) {
        std::uint64_t m = med(v);
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
    };

    std::string out = "{\n";
    out += "  \"bench\": \"buf_e0_bench\",\n";
    {
        std::string esc;
        json_escape(esc, cfg.label);
        out += "  \"label\": \"" + esc + "\",\n";
    }
    out += std::string("  \"phase\": \"") +
           (cfg.phase == Phase::A
                ? "A"
                : cfg.phase == Phase::B
                      ? "B"
                      : cfg.phase == Phase::C ? "C" : "D") +
           "\",\n";
    out += std::string("  \"arm\": \"") + arm_name(cfg.arm) + "\",\n";
    out += std::string("  \"regime\": \"") +
           (cfg.regime == Regime::pinned ? "pinned" : "arena") + "\",\n";
    out += "  \"size\": " + std::to_string(cfg.size) + ",\n";
    out += "  \"slots\": " + std::to_string(cfg.slots) + ",\n";
    out += "  \"reps\": " + std::to_string(cfg.reps) + ",\n";
    out += "  \"pages_per_buffer\": " + std::to_string(cfg.size / 4096) +
           ",\n";
    out += "  \"page_size\": 4096,\n";
    if (cfg.phase == Phase::C) {
        out += "  \"kc\": " + std::to_string(rs.kc) + ",\n";
        out += "  \"window_bytes\": " + std::to_string(rs.window_bytes) +
               ",\n";
        out += "  \"construct_ns\": " + std::to_string(rs.construct_ns) +
               ",\n";
        out += "  \"construct_minflt\": " +
               std::to_string(rs.construct_minflt) + ",\n";
        out += std::string("  \"construct_kind\": \"") + rs.construct_kind +
               "\",\n";
        out += "  \"prefault_ns\": " + std::to_string(rs.prefault_ns) + ",\n";
        out += "  \"prefault_minflt\": " + std::to_string(rs.prefault_minflt) +
               ",\n";
    }
    {
        std::string esc;
        json_escape(esc, cfg.file);
        out += "  \"file\": \"" + esc + "\",\n";
    }
    out += "  \"same_work\": {\"ops\": " + std::to_string(ops_total) +
           ", \"bytes\": " + std::to_string(bytes_total) +
           ", \"strong_checks\": " + std::to_string(rs.strong_checks) +
           ", \"ops_for_diff\": " + std::to_string(ops_for_diff(cfg, rs)) +
           ", \"ok\": " + (all_ok ? "true" : "false") + "},\n";
    out += stat_line("alloc_ns", alloc_ns) + ",\n";
    out += stat_line("io_ns", io_ns) + ",\n";
    out += stat_line("touch_ns", touch_ns) + ",\n";
    out += stat_line("destroy_ns", destroy_ns) + ",\n";
    out += stat_line("minflt_alloc", minflt_alloc) + ",\n";
    out += stat_line("minflt_io", minflt_io) + ",\n";
    out += stat_line("minflt_touch", minflt_touch) + ",\n";
    out += "  \"reps_detail\": [\n";
    for (std::size_t i = 0; i < rs.reps.size(); ++i) {
        const RepSpan& r = rs.reps[i];
        char b[512];
        std::snprintf(
            b, sizeof(b),
            "    {\"rep\": %llu, \"alloc_ns\": %llu, \"io_ns\": %llu, "
            "\"touch_ns\": %llu, \"destroy_ns\": %llu, \"minflt_alloc\": %lld, "
            "\"minflt_io\": %lld, \"minflt_touch\": %lld, "
            "\"majflt\": %lld, \"rss_before_kb\": %lld, \"rss_after_kb\": "
            "%lld, \"slot0_kind\": \"%s\", \"ops\": %llu, \"bytes\": %llu, "
            "\"ok\": %s}%s\n",
            (unsigned long long)i, (unsigned long long)r.alloc_ns,
            (unsigned long long)r.io_ns, (unsigned long long)r.touch_ns,
            (unsigned long long)r.destroy_ns, (long long)r.minflt_alloc,
            (long long)r.minflt_io, (long long)r.minflt_touch,
            (long long)r.majflt, (long long)r.rss_before_kb,
            (long long)r.rss_after_kb, r.slot0_kind,
            (unsigned long long)r.ops,
            (unsigned long long)r.bytes, r.ok ? "true" : "false",
            (i + 1 < rs.reps.size()) ? "," : "");
        out += b;
    }
    out += "  ],\n";
    out += "  \"all_reps_ok\": " + std::string(all_ok ? "true" : "false") +
           "\n}\n";
    std::fputs(out.c_str(), stdout);

    if (!all_ok) {
        std::fprintf(stderr,
                     "buf_e0_bench: same-work verification failed\n");
        return 3;
    }
    // Pinned fresh-page regime instrument check (fail-closed): if a fresh
    // construction came from the brk arena, the regime is silently broken
    // (allocator recycling) and the session must not be used. Phases A/B/D
    // carry per-rep evidence; phase C carries construct_kind instead (its
    // reps reuse the same buffers by design).
    if (cfg.regime == Regime::pinned && cfg.arm != Arm::b2) {
        if (cfg.phase != Phase::C) {
            for (const auto& r : rs.reps)
                if (std::strcmp(r.slot0_kind, "anon-mmap") != 0) {
                    std::fprintf(stderr,
                                 "buf_e0_bench: pinned regime broken: slot0="
                                 "%s (arm=%s phase=%c rep-set)\n",
                                 r.slot0_kind, arm_name(cfg.arm),
                                 "ABCD"[static_cast<int>(cfg.phase)]);
                    return 3;
                }
        }
        if (cfg.phase == Phase::C &&
            std::strcmp(rs.construct_kind, "anon-mmap") != 0) {
            std::fprintf(stderr,
                         "buf_e0_bench: pinned regime broken (C construct): "
                         "%s\n",
                         rs.construct_kind);
            return 3;
        }
    }
    return 0;
}
