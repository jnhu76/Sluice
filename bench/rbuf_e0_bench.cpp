// RBUF-E0 (#272, governing issue): research-only io_uring registered/fixed-
// buffer mechanism bench. NOT a production component — the production
// UringAsyncBackend has no registered-buffer capability (audit:
// research/rbuf-e0/RBUF-E0-AUDIT.md) and must not gain one from this file.
//
// Three arms over ONE shared engine (single submission/completion thread,
// identical ring setup, identical slot state machine, identical fixture,
// identical workload):
//   U0  ordinary-natural reference: per-slot heap vectors (natural policy,
//       no explicit alignment), ordinary READ/WRITE. Contextual baseline.
//   U1  causal ordinary control: ONE posix_memalign(4096) block, depth slots
//       of `chunk` bytes at chunk strides, ordinary READ/WRITE, reused
//       across all transfers.
//   U2  registered/fixed treatment: byte-identical storage as U1 plus
//       io_uring_register_buffers (one iovec per slot) and
//       READ_FIXED/WRITE_FIXED with buf_index = slot; unregister once at
//       lifecycle end.
// The ONLY U1 -> U2 delta is registration + fixed opcode selection. Any
// observed divergence beyond that is a causal-isolation failure (stop gate).
//
// Per run (`--run --arm A --chunk C --depth D --transfers H`): opens src and
// an O_TRUNC dst once, performs H back-to-back full-file READ+WRITE transfers
// (dst rewritten in place from offset 0; no re-open, no sync), and emits ONE
// JSON line with separated LIFECYCLE (alloc/register/unregister/setup/
// teardown ns) and STEADY-STATE (per-transfer ns) regions. Registration setup
// is NEVER inside a transfer span.
//
// Same-work gates are fail-closed (exit 3): exact CQE accounting
// (cqe_count == read_ops + write_ops == 2 * transfers * ceil(bytes/chunk)),
// every CQE res == requested length (short I/O is recorded, never retried),
// zero canceled/erroneous terminals, state-machine (slot/opcode/length)
// validation on every CQE, and no in-flight op left at transfer end.
// Registration/unregistration failures exit 4 (capability/lifecycle class).
//
// `--probe` verifies host capability behaviorally (register + fixed
// read/write round-trip on a small file) and prints a JSON capability line
// (exit 0 when fully capable, exit 5 otherwise).
//
// The driver (research/rbuf-e0/scripts/rbuf_e0.py) wraps each run under
// `perf stat -e instructions:u,cycles:u,task-clock`, hashes src/dst
// post-exit, and appends raw evidence into the immutable session.

#include <liburing.h>

#include <algorithm>
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
#include <sys/resource.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

constexpr std::size_t kBlock = 4096;
constexpr std::uint64_t kSeed = 0xE1E1E1E121212121ull;
constexpr std::size_t kAlign = 4096;
constexpr std::size_t kMaxDepth = 16;

[[noreturn]] void rbuf_fatal(const char* what, int err) {
    std::fprintf(stderr, "rbuf_e0_bench: fatal: %s (errno=%d: %s)\n", what,
                 err, std::strerror(err));
    std::exit(2);
}

[[noreturn]] void rbuf_semantic(const char* what) {
    std::fprintf(stderr, "rbuf_e0_bench: semantic failure: %s\n", what);
    std::exit(3);
}

[[noreturn]] void rbuf_register(const char* what, int err) {
    std::fprintf(stderr,
                 "rbuf_e0_bench: registration lifecycle failure: %s "
                 "(errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(4);
}

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct Config {
    std::string arm = "U1";
    std::size_t chunk = 1u << 21;
    std::size_t depth = 2;
    std::uint64_t transfers = 1;
    std::uint64_t file_bytes = 1ull << 30;
    std::string src, dst, label;
    bool generate = false;
    bool run = false;
    bool probe = false;
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* w) -> std::string {
            if (i + 1 >= argc) rbuf_fatal(w, EINVAL);
            return argv[++i];
        };
        if (a == "--arm") {
            c.arm = next("--arm");
        } else if (a == "--chunk") {
            c.chunk = std::strtoull(next("--chunk").c_str(), nullptr, 10);
        } else if (a == "--depth") {
            c.depth = std::strtoull(next("--depth").c_str(), nullptr, 10);
        } else if (a == "--transfers") {
            c.transfers =
                std::strtoull(next("--transfers").c_str(), nullptr, 10);
        } else if (a == "--file-bytes") {
            c.file_bytes =
                std::strtoull(next("--file-bytes").c_str(), nullptr, 10);
        } else if (a == "--src") {
            c.src = next("--src");
        } else if (a == "--dst") {
            c.dst = next("--dst");
        } else if (a == "--label") {
            c.label = next("--label");
        } else if (a == "--generate") {
            c.generate = true;
        } else if (a == "--run") {
            c.run = true;
        } else if (a == "--probe") {
            c.probe = true;
        } else {
            rbuf_semantic("unknown arg");
        }
    }
    return c;
}

// CHUNK-E0 canonical deterministic fixture (identical generator, identical
// seed -> identical sha256 as the research/chunk-e0 sessions).
void generate_file(const Config& cfg) {
    std::vector<std::byte> master(kBlock);
    auto* w = reinterpret_cast<std::uint64_t*>(master.data());
    for (std::size_t i = 0; i < kBlock / 8; ++i) w[i] = splitmix64(kSeed + i);
    int fd = ::open(cfg.src.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) rbuf_fatal("open(generate)", errno);
    std::vector<std::byte> chunkbuf(1u << 20);
    for (std::size_t off = 0; off < chunkbuf.size(); off += kBlock)
        std::memcpy(chunkbuf.data() + off, master.data(), kBlock);
    std::uint64_t written = 0;
    while (written < cfg.file_bytes) {
        std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunkbuf.size(), cfg.file_bytes - written));
        ssize_t x = ::write(fd, chunkbuf.data(), n);
        if (x < 0) {
            if (errno == EINTR) continue;
            rbuf_fatal("write(generate)", errno);
        }
        written += static_cast<std::uint64_t>(x);
    }
    if (::close(fd) != 0) rbuf_fatal("close(generate)", errno);
}

// Shared buffer storage. U0 = natural per-slot heap vectors; U1/U2 = ONE
// aligned block with chunk-strided slots (identical construction).
struct Storage {
    std::size_t chunk = 0;
    std::size_t depth = 0;
    std::byte* block = nullptr;                   // U1/U2
    std::vector<std::vector<std::byte>> natural;  // U0
    std::vector<iovec> iov;                       // U2 registration table

    std::byte* slot(std::size_t s) const { return block + s * chunk; }
};

Storage make_storage(const Config& cfg, std::uint64_t* alloc_ns,
                     std::size_t* align_remainder) {
    Storage st;
    st.chunk = cfg.chunk;
    st.depth = cfg.depth;
    const std::uint64_t t0 = now_ns();
    if (cfg.arm == "U0") {
        st.natural.resize(cfg.depth);
        for (std::size_t s = 0; s < cfg.depth; ++s)
            st.natural[s].resize(cfg.chunk);
        *align_remainder =
            reinterpret_cast<std::uintptr_t>(st.natural[0].data()) % kAlign;
    } else {
        void* p = nullptr;
        if (::posix_memalign(&p, kAlign, cfg.chunk * cfg.depth) != 0)
            rbuf_fatal("posix_memalign", ENOMEM);
        st.block = static_cast<std::byte*>(p);
        *align_remainder = reinterpret_cast<std::uintptr_t>(st.block) % kAlign;
    }
    *alloc_ns = now_ns() - t0;
    if (cfg.arm == "U2") {
        st.iov.resize(cfg.depth);
        for (std::size_t s = 0; s < cfg.depth; ++s) {
            st.iov[s].iov_base = st.slot(s);
            st.iov[s].iov_len = cfg.chunk;
        }
    }
    return st;
}

struct RunCounters {
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t read_ops = 0;
    std::uint64_t write_ops = 0;
    std::uint64_t cqe_count = 0;
    std::uint64_t canceled = 0;
    std::uint64_t errors = 0;
    std::uint64_t short_reads = 0;
    std::uint64_t short_writes = 0;
};

class Engine {
  public:
    Engine(const Config& cfg, int src_fd, int dst_fd, Storage& st,
           bool registered)
        : cfg_(cfg),
          st_(st),
          src_fd_(src_fd),
          dst_fd_(dst_fd),
          registered_(registered),
          chunks_((cfg.file_bytes + cfg.chunk - 1) / cfg.chunk),
          entries_(std::max<std::size_t>(8, 2 * cfg.depth)),
          in_flight_(cfg.depth, false),
          next_is_write_(cfg.depth, false),
          pending_is_write_(cfg.depth, false),
          pending_chunk_(cfg.depth, 0),
          pending_len_(cfg.depth, 0),
          next_chunk_(cfg.depth, 0) {}

    unsigned ring_entries_requested() const {
        return static_cast<unsigned>(entries_);
    }
    unsigned ring_entries_actual() const { return actual_entries_; }

    void init_ring() {
        const int rc =
            ::io_uring_queue_init(static_cast<unsigned>(entries_), &ring_,
                                  /*flags=*/0);
        if (rc != 0) rbuf_fatal("io_uring_queue_init", -rc);
        actual_entries_ = ring_.sq.ring_entries;
    }

    void register_buffers() {
        const int rc = ::io_uring_register_buffers(
            &ring_, st_.iov.data(), static_cast<unsigned>(st_.iov.size()));
        if (rc != 0) rbuf_register("io_uring_register_buffers", -rc);
    }

    void unregister_buffers() {
        const int rc = ::io_uring_unregister_buffers(&ring_);
        if (rc != 0) rbuf_register("io_uring_unregister_buffers", -rc);
    }

    void exit_ring() { ::io_uring_queue_exit(&ring_); }

    // One steady-state transfer: src[0..file_bytes) -> dst[0..file_bytes)
    // through the depth-slot read->write pipeline. Returns the span in ns.
    std::uint64_t run_transfer(RunCounters& c) {
        for (std::size_t s = 0; s < cfg_.depth; ++s) {
            in_flight_[s] = false;
            next_is_write_[s] = false;
            next_chunk_[s] = s;  // strided chunk assignment
        }
        ops_done_ = 0;
        target_ops_ = 2 * chunks_;

        const std::uint64_t t0 = now_ns();
        submit_pass(c);
        while (ops_done_ < target_ops_) {
            io_uring_cqe* cqe = nullptr;
            const int rc = ::io_uring_wait_cqe(&ring_, &cqe);
            if (rc < 0) rbuf_fatal("io_uring_wait_cqe", -rc);
            while (cqe != nullptr) {
                process_cqe(cqe, c);
                ::io_uring_cqe_seen(&ring_, cqe);
                cqe = nullptr;
                (void)::io_uring_peek_cqe(&ring_, &cqe);
            }
            if (ops_done_ < target_ops_) submit_pass(c);
        }
        for (std::size_t s = 0; s < cfg_.depth; ++s)
            if (in_flight_[s]) rbuf_semantic("in-flight op at transfer end");
        return now_ns() - t0;
    }

  private:
    std::uint64_t chunk_len(std::uint64_t chunk_idx) const {
        const std::uint64_t off = chunk_idx * cfg_.chunk;
        return std::min<std::uint64_t>(cfg_.chunk, cfg_.file_bytes - off);
    }

    void submit_pass(RunCounters& c) {
        unsigned prepared = 0;
        for (std::size_t s = 0; s < cfg_.depth; ++s) {
            if (in_flight_[s]) continue;
            const bool do_write = next_is_write_[s];
            std::uint64_t chunk_idx = 0;
            if (do_write) {
                chunk_idx = pending_chunk_[s];
            } else {
                chunk_idx = next_chunk_[s];
                if (chunk_idx >= chunks_) continue;  // slot drained
            }
            io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
                rbuf_semantic("SQE unavailable with entries >= 2*depth");
            const std::uint64_t off = chunk_idx * cfg_.chunk;
            const auto len = static_cast<unsigned>(chunk_len(chunk_idx));
            std::byte* buf =
                cfg_.arm == "U0" ? st_.natural[s].data() : st_.slot(s);
            if (do_write) {
                if (registered_)
                    ::io_uring_prep_write_fixed(sqe, dst_fd_, buf, len, off,
                                                static_cast<int>(s));
                else
                    ::io_uring_prep_write(sqe, dst_fd_, buf, len, off);
            } else {
                if (registered_)
                    ::io_uring_prep_read_fixed(sqe, src_fd_, buf, len, off,
                                               static_cast<int>(s));
                else
                    ::io_uring_prep_read(sqe, src_fd_, buf, len, off);
            }
            ::io_uring_sqe_set_data64(
                sqe, (static_cast<std::uint64_t>(s) << 1) | (do_write ? 1u
                                                                     : 0u));
            pending_chunk_[s] = chunk_idx;
            pending_len_[s] = len;
            pending_is_write_[s] = do_write;
            in_flight_[s] = true;
            // Advance the slot's schedule: after a read the next op for this
            // slot is the matching write; after a write it is the next read.
            next_is_write_[s] = !do_write;
            if (!do_write) next_chunk_[s] += cfg_.depth;
            ++prepared;
        }
        if (prepared == 0) {
            // All slots either in flight or drained: waiting on CQEs is the
            // correct next action. No-progress is only real when nothing is
            // ready AND nothing is outstanding while ops remain.
            if (ops_done_ < target_ops_) {
                for (std::size_t s = 0; s < cfg_.depth; ++s) {
                    if (!in_flight_[s]) continue;
                    return;  // progress pending on in-flight CQEs
                }
                rbuf_semantic("no progress possible (no ready slot, "
                              "no in-flight op)");
            }
            return;
        }
        const int rc = ::io_uring_submit(&ring_);
        if (rc < 0) rbuf_fatal("io_uring_submit", -rc);
        if (static_cast<unsigned>(rc) != prepared)
            rbuf_semantic("short io_uring_submit");
    }

    void process_cqe(io_uring_cqe* cqe, RunCounters& c) {
        const std::uint64_t ud = cqe->user_data;
        const std::size_t s = static_cast<std::size_t>(ud >> 1);
        const bool is_write = (ud & 1u) != 0;
        if (s >= cfg_.depth) rbuf_semantic("CQE user_data slot out of range");
        if (!in_flight_[s] || pending_is_write_[s] != is_write)
            rbuf_semantic("CQE does not match slot state machine");
        ++c.cqe_count;
        if (cqe->res < 0) {
            ++c.errors;
            if (cqe->res == -ECANCELED) ++c.canceled;
            rbuf_semantic("unexpected error terminal on data-path CQE");
        }
        if (static_cast<std::uint64_t>(cqe->res) != pending_len_[s]) {
            if (is_write)
                ++c.short_writes;
            else
                ++c.short_reads;
            rbuf_semantic("short I/O on data-path CQE (recorded, not retried)");
        }
        if (is_write) {
            c.bytes_written += static_cast<std::uint64_t>(cqe->res);
            ++c.write_ops;
        } else {
            c.bytes_read += static_cast<std::uint64_t>(cqe->res);
            ++c.read_ops;
        }
        ++ops_done_;
        in_flight_[s] = false;
    }

    const Config& cfg_;
    Storage& st_;
    int src_fd_;
    int dst_fd_;
    bool registered_;
    std::uint64_t chunks_;
    std::size_t entries_;
    unsigned actual_entries_ = 0;
    io_uring ring_{};
    std::vector<bool> in_flight_;
    std::vector<bool> next_is_write_;
    std::vector<bool> pending_is_write_;
    std::vector<std::uint64_t> pending_chunk_;
    std::vector<std::uint64_t> pending_len_;
    std::vector<std::uint64_t> next_chunk_;
    std::uint64_t ops_done_ = 0;
    std::uint64_t target_ops_ = 0;
};

// ---- probe: behavioral host-capability check (small scratch files) --------

int run_probe(const Config& cfg) {
    if (cfg.src.empty() || cfg.dst.empty())
        rbuf_semantic("probe requires --src/--dst scratch paths");
    std::vector<std::byte> master(kBlock);
    auto* w = reinterpret_cast<std::uint64_t*>(master.data());
    for (std::size_t i = 0; i < kBlock / 8; ++i) w[i] = splitmix64(kSeed + i);
    const int sfd = ::open(cfg.src.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sfd < 0) rbuf_fatal("open(probe src)", errno);
    ssize_t x = ::write(sfd, master.data(), kBlock);
    if (x < 0) rbuf_fatal("write(probe src)", errno);
    if (::close(sfd) != 0) rbuf_fatal("close(probe src)", errno);

    int init_errno = 0, reg_errno = 0, unreg_errno = 0;
    int read_res = 0, write_res = 0;
    bool read_ok = false, write_ok = false;
    io_uring ring{};
    const int irc = ::io_uring_queue_init(8, &ring, 0);
    if (irc != 0) {
        init_errno = -irc;
    } else {
        void* p = nullptr;
        if (::posix_memalign(&p, kAlign, kBlock) != 0)
            rbuf_fatal("posix_memalign(probe)", ENOMEM);
        auto* buf = static_cast<std::byte*>(p);
        const iovec iov{p, kBlock};
        const int rrc = ::io_uring_register_buffers(&ring, &iov, 1);
        if (rrc != 0) {
            reg_errno = -rrc;
        } else {
            const int rfd = ::open(cfg.src.c_str(), O_RDONLY);
            const int wfd =
                ::open(cfg.dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (rfd < 0) rbuf_fatal("open(probe read)", errno);
            if (wfd < 0) rbuf_fatal("open(probe write)", errno);
            io_uring_sqe* sq = ::io_uring_get_sqe(&ring);
            ::io_uring_prep_read_fixed(sq, rfd, buf, kBlock, 0, 0);
            ::io_uring_sqe_set_data64(sq, 1);
            io_uring_sqe* sw = ::io_uring_get_sqe(&ring);
            ::io_uring_prep_write_fixed(sw, wfd, buf, kBlock, 0, 0);
            ::io_uring_sqe_set_data64(sw, 2);
            const int src_rc = ::io_uring_submit(&ring);
            if (src_rc != 2) rbuf_semantic("probe submit");
            int seen = 0;
            while (seen < 2) {
                io_uring_cqe* cqe = nullptr;
                const int wrc = ::io_uring_wait_cqe(&ring, &cqe);
                if (wrc < 0) rbuf_fatal("io_uring_wait_cqe(probe)", -wrc);
                if (cqe->user_data == 1) {
                    read_res = cqe->res;
                    read_ok = read_res == static_cast<int>(kBlock) &&
                              std::memcmp(buf, master.data(), kBlock) == 0;
                    ++seen;
                } else if (cqe->user_data == 2) {
                    write_res = cqe->res;
                    ++seen;
                } else {
                    rbuf_semantic("probe: unknown CQE cookie");
                }
                ::io_uring_cqe_seen(&ring, cqe);
            }
            ::close(rfd);
            ::close(wfd);
            char back[kBlock];
            const int vfd = ::open(cfg.dst.c_str(), O_RDONLY);
            if (vfd < 0) rbuf_fatal("open(probe verify)", errno);
            const ssize_t g = ::pread(vfd, back, kBlock, 0);
            const bool file_ok =
                g == static_cast<ssize_t>(kBlock) &&
                std::memcmp(back, master.data(), kBlock) == 0;
            ::close(vfd);
            write_ok = write_res == static_cast<int>(kBlock) && file_ok;
            const int urc = ::io_uring_unregister_buffers(&ring);
            if (urc != 0) unreg_errno = -urc;
        }
        ::free(p);
        ::io_uring_queue_exit(&ring);
    }
    struct rlimit rl;
    if (::getrlimit(RLIMIT_MEMLOCK, &rl) != 0)
        rbuf_fatal("getrlimit(RLIMIT_MEMLOCK)", errno);
    const bool ok = init_errno == 0 && reg_errno == 0 && unreg_errno == 0 &&
                    read_ok && write_ok;
    std::printf(
        "{\"bench\":\"rbuf_e0_bench\",\"mode\":\"probe\","
        "\"uring_queue_init_errno\":%d,\"register_errno\":%d,"
        "\"unregister_errno\":%d,\"read_fixed_res\":%d,"
        "\"write_fixed_res\":%d,\"read_content_ok\":%s,"
        "\"write_content_ok\":%s,\"memlock_cur_bytes\":%llu,"
        "\"memlock_max_bytes\":%llu,\"page_size\":%ld,\"capable\":%s}\n",
        init_errno, reg_errno, unreg_errno, read_res, write_res,
        read_ok ? "true" : "false", write_ok ? "true" : "false",
        (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max,
        ::sysconf(_SC_PAGESIZE), ok ? "true" : "false");
    return ok ? 0 : 5;
}

// ---- formal run ------------------------------------------------------------

int run_one(const Config& cfg) {
    if (cfg.arm != "U0" && cfg.arm != "U1" && cfg.arm != "U2")
        rbuf_semantic("arm must be U0|U1|U2");
    if (cfg.chunk == 0 || cfg.depth == 0 || cfg.transfers == 0)
        rbuf_semantic("chunk/depth/transfers must be > 0");
    if (cfg.depth > kMaxDepth) rbuf_semantic("depth > 16 unsupported");
    if (cfg.arm != "U0" && cfg.chunk % kAlign != 0)
        rbuf_semantic("chunk must be 4096-aligned for U1/U2 slot strides");

    struct rusage ru0, ru1;
    if (::getrusage(RUSAGE_SELF, &ru0) != 0)
        rbuf_fatal("getrusage(before)", errno);

    const std::uint64_t t_setup0 = now_ns();
    const int src_fd = ::open(cfg.src.c_str(), O_RDONLY);
    if (src_fd < 0) rbuf_fatal("open(src)", errno);
    const int dst_fd =
        ::open(cfg.dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) rbuf_fatal("open(dst)", errno);

    std::uint64_t alloc_ns = 0;
    std::size_t align_remainder = 0;
    Storage st = make_storage(cfg, &alloc_ns, &align_remainder);
    // Slot address evidence captured while storage is alive.
    std::string addrs = "[";
    for (std::size_t s = 0; s < cfg.depth; ++s) {
        const void* p = cfg.arm == "U0"
                            ? static_cast<const void*>(st.natural[s].data())
                            : static_cast<const void*>(st.slot(s));
        char hex[32];
        std::snprintf(hex, sizeof hex, "\"%p\"", p);
        if (s) addrs += ",";
        addrs += hex;
    }
    addrs += "]";

    Engine engine(cfg, src_fd, dst_fd, st, /*registered=*/cfg.arm == "U2");
    engine.init_ring();
    const std::uint64_t t_reg0 = now_ns();
    if (cfg.arm == "U2") engine.register_buffers();
    const std::uint64_t register_ns = now_ns() - t_reg0;
    const std::uint64_t setup_ns = now_ns() - t_setup0;

    RunCounters counters;
    std::vector<std::uint64_t> transfer_ns;
    transfer_ns.reserve(cfg.transfers);
    for (std::uint64_t h = 0; h < cfg.transfers; ++h)
        transfer_ns.push_back(engine.run_transfer(counters));

    const std::uint64_t t_td0 = now_ns();
    std::uint64_t unregister_ns = 0;
    if (cfg.arm == "U2") {
        const std::uint64_t t_un0 = now_ns();
        engine.unregister_buffers();
        unregister_ns = now_ns() - t_un0;
    }
    if (::close(src_fd) != 0) rbuf_fatal("close(src)", errno);
    if (::close(dst_fd) != 0) rbuf_fatal("close(dst)", errno);
    engine.exit_ring();
    if (cfg.arm != "U0" && st.block != nullptr) {
        ::free(st.block);
        st.block = nullptr;
    }
    const std::uint64_t teardown_ns = now_ns() - t_td0;

    // ---- same-work gates (fail-closed) ----
    const std::uint64_t chunks = (cfg.file_bytes + cfg.chunk - 1) / cfg.chunk;
    const std::uint64_t expect_bytes = cfg.file_bytes * cfg.transfers;
    const std::uint64_t expect_ops = chunks * cfg.transfers;
    if (counters.bytes_read != expect_bytes)
        rbuf_semantic("bytes_read != transfers * file size");
    if (counters.bytes_written != expect_bytes)
        rbuf_semantic("bytes_written != transfers * file size");
    if (counters.read_ops != expect_ops || counters.write_ops != expect_ops)
        rbuf_semantic("op counts != transfers * ceil(bytes/chunk)");
    if (counters.cqe_count != 2 * expect_ops)
        rbuf_semantic("CQE count != read_ops + write_ops");
    if (counters.canceled != 0)
        rbuf_semantic("unexpected canceled terminal observed");
    if (counters.errors != 0) rbuf_semantic("error terminal observed");

    if (::getrusage(RUSAGE_SELF, &ru1) != 0)
        rbuf_fatal("getrusage(after)", errno);
    const std::uint64_t utime_us =
        static_cast<std::uint64_t>(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) *
            1000000ull +
        static_cast<std::uint64_t>(ru1.ru_utime.tv_usec -
                                   ru0.ru_utime.tv_usec);
    const std::uint64_t stime_us =
        static_cast<std::uint64_t>(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) *
            1000000ull +
        static_cast<std::uint64_t>(ru1.ru_stime.tv_usec -
                                   ru0.ru_stime.tv_usec);

    std::uint64_t steady_total = 0;
    for (std::uint64_t v : transfer_ns) steady_total += v;

    std::printf(
        "{\"bench\":\"rbuf_e0_bench\",\"label\":\"%s\",\"arm\":\"%s\","
        "\"module\":\"uring-direct\",\"workers\":1,"
        "\"chunk\":%llu,\"depth\":%llu,\"transfers\":%llu,"
        "\"file_bytes\":%llu,\"chunks_per_transfer\":%llu,"
        "\"ring_entries_requested\":%u,\"ring_entries\":%u,"
        "\"alloc_ns\":%llu,\"register_ns\":%llu,\"unregister_ns\":%llu,"
        "\"setup_ns\":%llu,\"teardown_ns\":%llu,\"transfer_ns\":[",
        cfg.label.c_str(), cfg.arm.c_str(), (unsigned long long)cfg.chunk,
        (unsigned long long)cfg.depth, (unsigned long long)cfg.transfers,
        (unsigned long long)cfg.file_bytes, (unsigned long long)chunks,
        engine.ring_entries_requested(), engine.ring_entries_actual(),
        (unsigned long long)alloc_ns, (unsigned long long)register_ns,
        (unsigned long long)unregister_ns, (unsigned long long)setup_ns,
        (unsigned long long)teardown_ns);
    for (std::size_t i = 0; i < transfer_ns.size(); ++i)
        std::printf("%s%llu", i ? "," : "", (unsigned long long)transfer_ns[i]);
    std::printf(
        "],\"steady_total_ns\":%llu,"
        "\"bytes_read\":%llu,\"bytes_written\":%llu,"
        "\"read_ops\":%llu,\"write_ops\":%llu,\"cqe_count\":%llu,"
        "\"canceled\":%llu,\"errors\":%llu,"
        "\"short_reads\":%llu,\"short_writes\":%llu,"
        "\"utime_us\":%llu,\"stime_us\":%llu,\"maxrss_kb\":%ld,"
        "\"minflt\":%ld,\"majflt\":%ld,"
        "\"align_remainder\":%zu,\"slot_stride\":%llu,"
        "\"registered_buffers\":%llu,\"registered_bytes\":%llu,"
        "\"slot_addrs\":%s,\"ok\":true}\n",
        (unsigned long long)steady_total,
        (unsigned long long)counters.bytes_read,
        (unsigned long long)counters.bytes_written,
        (unsigned long long)counters.read_ops,
        (unsigned long long)counters.write_ops,
        (unsigned long long)counters.cqe_count,
        (unsigned long long)counters.canceled,
        (unsigned long long)counters.errors,
        (unsigned long long)counters.short_reads,
        (unsigned long long)counters.short_writes,
        (unsigned long long)utime_us, (unsigned long long)stime_us,
        ru1.ru_maxrss, ru1.ru_minflt, ru1.ru_majflt, align_remainder,
        (unsigned long long)(cfg.arm == "U0" ? 0 : cfg.chunk),
        (unsigned long long)(cfg.arm == "U2" ? cfg.depth : 0),
        (unsigned long long)(cfg.arm == "U2" ? cfg.chunk * cfg.depth : 0),
        addrs.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    if (cfg.src.empty()) rbuf_semantic("--src required");
    if (cfg.generate) {
        generate_file(cfg);
        std::printf("generated %s (%llu bytes)\n", cfg.src.c_str(),
                    (unsigned long long)cfg.file_bytes);
        return 0;
    }
    if (cfg.probe) return run_probe(cfg);
    if (cfg.dst.empty()) rbuf_semantic("--dst required");
    if (!cfg.run) rbuf_semantic("--run required (or --generate/--probe)");
    return run_one(cfg);
}
