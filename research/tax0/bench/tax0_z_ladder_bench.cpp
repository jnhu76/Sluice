// TAX-0B semantic-floor Z-ladder bench (#250 / #259 / PR #260).
//
// EXPERIMENT QUESTION (preregistered in research/tax0/
// TAX0-A2-CONTROL-PLANE-SEMANTIC-FLOOR.md + census
// `z_ladder_preregistration` / `seams[F05].z1b_design`, frozen @ 9670224):
// if an expert hand-wrote the same required semantics directly over
// io_uring, what measurable control-plane work would disappear from Sluice?
//
//   Z1   raw liburing minimal (bare floor; kernel-guaranteed identity only)
//   Z1b  minimal semantic-equivalent uring — the frozen F05 checklist made
//        explicit userspace machinery (SEMANTIC FLOOR)
//   Z1bw Z1b + one continuation consumer (lost-wake-safe one-shot wait on the
//        request's own terminal state, reaper-thread publication) — the
//        hand-written counterpart of the Z3 continuation path
//   Z2   AsyncIoContext + UringAsyncBackend, manual driver, NO Scheduler
//   Z3   ApplicationRuntime + RuntimeTaskContext::await_completion
//
//   capability_cost = Z1b - Z1;  abstraction_tax = Z2/Z3 - Z1b;
//   Z1 - anything is NOT a valid tax measure (census `definitions`).
//
// SAME-WORK (all arms, frozen 13-item list): identical fd/offset sequence/
// bytes/op count/in-flight depth D (== request_capacity == ring queue depth,
// so no dispatch backlog)/buffer reuse policy (slot-local buffers, reuse only
// after that slot's terminal is consumed)/file/cache state/durability policy
// (no fsync — writeback only, outside the router of this question)/
// completion count (exactly ops per rep)/error policy (any deviation aborts
// exit 3). The WRITE arm refills each buffer from the same splitmix64 master
// block (kSeed shared with EXP-0/U0/shootout harnesses) before every submit
// and verifies the full file word-sum after the last rep; the READ arm
// verifies each buffer's word-sum inline (uniform validation work in every
// arm, per the shootout precedent).
//
// LIFECYCLE SEPARATION (census rule): ring/context/runtime construction,
// buffer allocation, and file preparation happen once BEFORE the measured
// reps; each rep is pure steady-state op traffic. Warmup reps are reported
// separately from measured reps.
//
// Z1b checklist binding (census `seams[F05].z1b_design.required_semantics`):
//   bounded in-flight            fixed slot table, window submit-consume < D,
//                                explicit outstanding counter (max witnessed)
//   stable request identity      never-reused cookie = op sequence number
//                                (equivalently generation-tagged slot:
//                                cookie = gen*D + slot)
//   stale-completion protection  CQE whose cookie does not name the slot's
//                                current occupant is dropped, never delivered
//   exactly-once publication     IN_FLIGHT -> TERMINAL single transition;
//                                a second terminal for the same cookie is a
//                                harness failure
//   safe buffer lifetime         slot-local buffer, refilled only at submit
//                                of the slot's current occupant, consumed
//                                before the slot returns to EMPTY
//   one continuation (Z1bw)      consumer parks on the per-slot terminal
//                                predicate under a mutex (lost-wake-safe
//                                commit-to-wait protocol); a reaper thread
//                                publishes terminal + notify
//   no per-op heap allocation    all state is fixed preallocated arrays
//
// This target links the PRODUCTION sluice_async (no internal-testing seams):
// Z2/Z3 measure production behavior as-built. Requires --with-liburing;
// without a real ring it compiles to a fail-closed stub (never a fake arm).
//
// Output: one JSON object on stdout. Exit 0 only when every counted
// repetition completed with exact op/byte/word-sum accounting.

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

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>
#endif

namespace {

#if defined(SLUICE_HAS_LIBURING)

// ---------------------------------------------------------------------------
// Fail-closed helpers (shared shape with tax0_capacity_bench/shootout)
// ---------------------------------------------------------------------------

[[noreturn]] void bench_fatal(const char* what, int err) {
    std::fprintf(stderr, "tax0_z_ladder_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(3);
}

[[noreturn]] void bench_semantic(const char* what) {
    std::fprintf(stderr, "tax0_z_ladder_bench: semantic failure: %s\n", what);
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

// ---------------------------------------------------------------------------
// Deterministic workload bytes — IDENTICAL generator to tax0_capacity_bench /
// tax0u0_router_bench / shootout (same kSeed, same splitmix64 master block).
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

enum class Op { read, write };
enum class Arm { z1, z1b, z1bw, z2, z3 };

struct Config {
    Arm arm = Arm::z1;
    Op op = Op::read;
    std::size_t request_size = 4096;
    std::size_t total_bytes = 64u << 20;
    std::size_t depth = 8;
    unsigned workers = 1;
    std::size_t reps = 1;
    std::size_t warmup = 1;
    bool self_check = false;
    bool runner_verify = false;  // defer write verification to the session
                                 // runner (keeps it out of perf windows)
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
    if (c.arm != Arm::z3 && c.workers != 1) {
        err = "--workers applies to the z3 arm only (z1/z1b/z1bw/z2 arms are "
              "single-driver by construction)";
        return false;
    }
    if (c.workers == 0 || c.workers > 64) {
        err = "--workers must be in [1, 64]";
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
    if (!c.self_check && c.file.empty()) {
        err = "--file is required (unless --self-check)";
        return false;
    }
    return true;
}

// One measured (or warmup) repetition. `terminals` counts z1bw reaper
// publications; the accounting witnesses are the Z1b/Z1bw machinery state.
struct RepResult {
    std::uint64_t wall_ns = 0;
    std::uint64_t user_ns = 0;
    std::uint64_t sys_ns = 0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    std::uint64_t word_sum = 0;
    std::uint64_t errors = 0;
    // Z1b/Z1bw witnesses (zero for other arms).
    std::uint64_t stale_dropped = 0;
    std::uint64_t outstanding_max = 0;
};

struct RunState {
    Config cfg;
    std::vector<std::uint64_t> master_words;
    std::uint64_t expected_sum = 0;
    std::size_t ops = 0;
    int fd = -1;
    std::vector<std::vector<std::byte>> buf;

    const std::byte* master_bytes() const {
        return reinterpret_cast<const std::byte*>(master_words.data());
    }
    void init_buffers() {
        buf.resize(cfg.depth);
        for (auto& b : buf) b.assign(cfg.request_size, std::byte{0});
    }
};

// ---------------------------------------------------------------------------
// File preparation / write verification (shootout pattern, shared bytes)
// ---------------------------------------------------------------------------

void raw_pread_exact(int fd, std::byte* dst, std::size_t len,
                     std::uint64_t off) {
    ssize_t n = sluice::detail::retry_on_eintr(
        [&] { return ::pread(fd, dst, len, static_cast<off_t>(off)); });
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

// ---------------------------------------------------------------------------
// Z1b / Z1bw slot state machine — the frozen F05 checklist as explicit
// machinery. Fixed arrays, no per-op heap allocation.
// ---------------------------------------------------------------------------

enum class SlotState : std::uint8_t { empty = 0, in_flight = 1, terminal = 2 };

struct Z1bSlot {
    std::uint64_t cookie = 0;  // never-reused op sequence number
    SlotState state = SlotState::empty;
    ssize_t res = 0;
};

struct Z1bWitness {
    std::uint64_t stale_dropped = 0;
    std::uint64_t outstanding = 0;
    std::uint64_t outstanding_max = 0;
    std::uint64_t terminals = 0;  // total CQE publications accepted
};

// CQE acceptance: single IN_FLIGHT -> TERMINAL winner; anything whose cookie
// does not name the slot's current in-flight occupant is stale and is
// dropped, never delivered. Used by the pure state-machine self-check too.
// Returns true when the CQE was accepted as this request's terminal.
bool z1b_cqe_terminal(Z1bSlot& s, std::uint64_t cookie, ssize_t res,
                      Z1bWitness& w, bool fatal_on_double) {
    if (s.state != SlotState::in_flight || s.cookie != cookie) {
        ++w.stale_dropped;
        return false;
    }
    if (s.state == SlotState::terminal) {
        // Unreachable: in_flight check above. Kept as the explicit
        // exactly-once guard for the self-check double-terminal case.
        if (fatal_on_double) bench_semantic("exactly-once violated (double terminal)");
        return false;
    }
    s.state = SlotState::terminal;
    s.res = res;
    ++w.terminals;
    return true;
}

// ---------------------------------------------------------------------------
// Arms
// ---------------------------------------------------------------------------

// --- Z1: raw liburing bare floor -------------------------------------------
// Competent minimal: batched prep + io_uring_submit, batched reap
// (peek_batch_cqe + cqe_advance), FIFO window for in-flight depth, slot-index
// user_data (kernel delivers it verbatim — that guarantee IS the floor's
// identity mechanism). No generation, no state machine, no accounting
// counter, no stale handling: a bare floor leans on the kernel 1:1
// SQE->CQE contract.
RepResult z1_run(RunState& rs, io_uring& ring) {
    const Config& c = rs.cfg;
    const std::size_t D = c.depth;
    std::vector<ssize_t> res(D, 0);
    std::vector<std::uint8_t> done(D, 0);
    RepResult r;
    std::uint64_t submit_k = 0, consume_k = 0;
    while (consume_k < rs.ops) {
        while (submit_k < rs.ops && submit_k - consume_k < D) {
            std::size_t slot = submit_k % D;
            std::byte* b = rs.buf[slot].data();
            std::uint64_t off =
                static_cast<std::uint64_t>(submit_k) * c.request_size;
            io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            if (sqe == nullptr) break;  // SQ ring full -> flush below
            if (c.op == Op::write)
                fill_from_master(b, c.request_size, rs.master_bytes());
            if (c.op == Op::read)
                io_uring_prep_read(sqe, rs.fd, b, c.request_size, off);
            else
                io_uring_prep_write(sqe, rs.fd, b, c.request_size, off);
            io_uring_sqe_set_data64(sqe, static_cast<std::uint64_t>(slot));
            ++submit_k;
        }
        // SQEs stay staged; the reap-phase submit_and_wait below flushes
        // them in the same enter (single enter per op, mirroring the
        // production driver structure).
        while (!done[consume_k % D]) {
            int ret = ::io_uring_submit_and_wait(&ring, 1);
            if (ret == -EINTR) continue;
            if (ret < 0) bench_fatal("io_uring_submit_and_wait", -ret);
            io_uring_cqe* cqes[32];
            unsigned n = ::io_uring_peek_batch_cqe(&ring, cqes, 32);
            for (unsigned i = 0; i < n; ++i) {
                unsigned slot = static_cast<unsigned>(
                    ::io_uring_cqe_get_data64(cqes[i]));
                res[slot] = cqes[i]->res;
                done[slot] = 1;
            }
            if (n > 0) ::io_uring_cq_advance(&ring, n);
        }
        std::size_t slot = consume_k % D;
        if (res[slot] != static_cast<ssize_t>(c.request_size))
            bench_semantic("z1: op moved wrong byte count");
        if (c.op == Op::read)
            r.word_sum += word_sum(rs.buf[slot].data(), c.request_size);
        done[slot] = 0;
        ++consume_k;
        ++r.ops;
        r.bytes += static_cast<std::uint64_t>(c.request_size);
    }
    return r;
}

// --- Z1b / Z1bw shared driver state ----------------------------------------
struct Z1bShared {
    io_uring* ring = nullptr;
    RunState* rs = nullptr;
    std::vector<Z1bSlot> slots;
    Z1bWitness w;
    std::mutex mtx;              // guards slots/w (z1bw); unused leaf in z1b
    std::condition_variable cv;  // continuation wake (z1bw only)
};

// CQE drain shared by z1b (driver thread) and z1bw (reaper thread).
void z1b_drain_cqes(Z1bShared& sh) {
    io_uring_cqe* cqes[32];
    unsigned n = ::io_uring_peek_batch_cqe(sh.ring, cqes, 32);
    if (n == 0) return;
    for (unsigned i = 0; i < n; ++i) {
        std::uint64_t cookie = ::io_uring_cqe_get_data64(cqes[i]);
        std::size_t slot = static_cast<std::size_t>(cookie % sh.slots.size());
        z1b_cqe_terminal(sh.slots[slot], cookie, cqes[i]->res, sh.w,
                         /*fatal_on_double=*/true);
    }
    ::io_uring_cq_advance(sh.ring, n);
}

// --- Z1b: minimal semantic-equivalent uring (driver style) -----------------
RepResult z1b_run(Z1bShared& sh) {
    RunState& rs = *sh.rs;
    const Config& c = rs.cfg;
    const std::size_t D = c.depth;
    RepResult r;
    std::uint64_t submit_k = 0, consume_k = 0;
    while (consume_k < rs.ops) {
        while (submit_k < rs.ops && submit_k - consume_k < D) {
            std::size_t slot = submit_k % D;
            Z1bSlot& s = sh.slots[slot];
            if (s.state != SlotState::empty)
                bench_semantic("z1b: admission on non-empty slot");
            std::byte* b = rs.buf[slot].data();
            std::uint64_t off =
                static_cast<std::uint64_t>(submit_k) * c.request_size;
            if (c.op == Op::write)
                fill_from_master(b, c.request_size, rs.master_bytes());
            io_uring_sqe* sqe = io_uring_get_sqe(sh.ring);
            if (sqe == nullptr) break;
            if (c.op == Op::read)
                io_uring_prep_read(sqe, rs.fd, b, c.request_size, off);
            else
                io_uring_prep_write(sqe, rs.fd, b, c.request_size, off);
            io_uring_sqe_set_data64(sqe, submit_k);  // never-reused cookie
            s.cookie = submit_k;
            s.state = SlotState::in_flight;
            ++sh.w.outstanding;
            sh.w.outstanding_max =
                std::max(sh.w.outstanding_max, sh.w.outstanding);
            ++submit_k;
        }
        // SQEs stay staged; the reap-phase submit_and_wait flushes them in
        // the same enter (single enter per op).
        // Consume FIFO: wait until THIS op's terminal is published.
        {
            std::size_t slot = consume_k % D;
            std::uint64_t expect = consume_k;
            Z1bSlot& s = sh.slots[slot];
            while (!(s.state == SlotState::terminal && s.cookie == expect)) {
                int ret = ::io_uring_submit_and_wait(sh.ring, 1);
                if (ret == -EINTR) continue;
                if (ret < 0) bench_fatal("io_uring_submit_and_wait", -ret);
                z1b_drain_cqes(sh);
            }
            if (s.cookie != expect || s.res != static_cast<ssize_t>(c.request_size))
                bench_semantic("z1b: terminal mismatch or wrong byte count");
            if (c.op == Op::read)
                r.word_sum += word_sum(rs.buf[slot].data(), c.request_size);
            s.state = SlotState::empty;  // ownership recovered; buffer reusable
            --sh.w.outstanding;
            ++consume_k;
            ++r.ops;
            r.bytes += static_cast<std::uint64_t>(c.request_size);
        }
    }
    if (sh.w.outstanding != 0) bench_semantic("z1b: outstanding residue");
    r.stale_dropped = sh.w.stale_dropped;
    r.outstanding_max = sh.w.outstanding_max;
    return r;
}

// --- Z1bw: Z1b + one continuation consumer ---------------------------------
// Reaper thread: wait_cqe -> drain -> publish terminal + notify.
// Consumer (main): parks on the per-slot terminal predicate under the shared
// mutex — the lost-wake-safe one-shot wait on the request's own completion
// state. No cancellation, no multi-waiter: exactly what the Z3 comparison
// cell purchases.
void z1bw_reaper(Z1bShared& sh) {
    const std::uint64_t total_ops = sh.rs->ops;
    while (true) {
        {
            std::lock_guard<std::mutex> lk(sh.mtx);
            if (sh.w.terminals == total_ops) return;
        }
        io_uring_cqe* cqe = nullptr;  // wait only; the batch drain advances
        int ret = ::io_uring_wait_cqe(sh.ring, &cqe);
        if (ret == -EINTR) continue;
        if (ret < 0) bench_fatal("io_uring_wait_cqe", -ret);
        std::lock_guard<std::mutex> lk(sh.mtx);
        z1b_drain_cqes(sh);
        sh.cv.notify_one();
    }
}

RepResult z1bw_run(Z1bShared& sh) {
    sh.w = Z1bWitness{};  // per-rep witness: the reaper's exit predicate
                          // reads terminals — a stale count from a previous
                          // rep makes it exit before reaping (deadlock).
    RunState& rs = *sh.rs;
    const Config& c = rs.cfg;
    const std::size_t D = c.depth;
    RepResult r;
    std::uint64_t submit_k = 0, consume_k = 0;
    std::thread reaper([&] { z1bw_reaper(sh); });
    while (consume_k < rs.ops) {
        std::size_t prepped = 0;
        while (submit_k < rs.ops && submit_k - consume_k < D) {
            std::size_t slot = submit_k % D;
            std::byte* b = rs.buf[slot].data();
            std::uint64_t off =
                static_cast<std::uint64_t>(submit_k) * c.request_size;
            std::unique_lock<std::mutex> lk(sh.mtx);
            Z1bSlot& s = sh.slots[slot];
            if (s.state != SlotState::empty)
                bench_semantic("z1bw: admission on non-empty slot");
            if (c.op == Op::write)
                fill_from_master(b, c.request_size, rs.master_bytes());
            io_uring_sqe* sqe = io_uring_get_sqe(sh.ring);
            if (sqe == nullptr) break;
            if (c.op == Op::read)
                io_uring_prep_read(sqe, rs.fd, b, c.request_size, off);
            else
                io_uring_prep_write(sqe, rs.fd, b, c.request_size, off);
            io_uring_sqe_set_data64(sqe, submit_k);
            s.cookie = submit_k;
            s.state = SlotState::in_flight;
            ++sh.w.outstanding;
            sh.w.outstanding_max =
                std::max(sh.w.outstanding_max, sh.w.outstanding);
            ++submit_k;
            ++prepped;
            lk.unlock();
        }
        if (prepped > 0) {
            int ret = ::io_uring_submit(sh.ring);
            if (ret < 0) bench_fatal("io_uring_submit", -ret);
        }
        {
            std::size_t slot = consume_k % D;
            std::uint64_t expect = consume_k;
            std::unique_lock<std::mutex> lk(sh.mtx);
            sh.cv.wait(lk, [&] {
                Z1bSlot& s = sh.slots[slot];
                return s.state == SlotState::terminal && s.cookie == expect;
            });
            Z1bSlot& s = sh.slots[slot];
            if (s.res != static_cast<ssize_t>(c.request_size))
                bench_semantic("z1bw: terminal with wrong byte count");
            if (c.op == Op::read)
                r.word_sum += word_sum(rs.buf[slot].data(), c.request_size);
            s.state = SlotState::empty;
            --sh.w.outstanding;
            ++consume_k;
            ++r.ops;
            r.bytes += static_cast<std::uint64_t>(c.request_size);
        }
    }
    reaper.join();
    if (sh.w.outstanding != 0) bench_semantic("z1bw: outstanding residue");
    r.stale_dropped = sh.w.stale_dropped;
    r.outstanding_max = sh.w.outstanding_max;
    return r;
}

// --- Z2: AsyncIoContext + UringAsyncBackend, manual driver, no Scheduler ---
RepResult z2_run(RunState& rs, sluice::async::AsyncIoContext& ctx,
                 std::vector<sluice::async::Completion<std::size_t>>& comp) {
    const Config& c = rs.cfg;
    const std::size_t D = c.depth;
    RepResult r;
    std::uint64_t submit_k = 0, consume_k = 0;
    while (consume_k < rs.ops) {
        while (submit_k < rs.ops && submit_k - consume_k < D) {
            std::size_t slot = submit_k % D;
            std::byte* b = rs.buf[slot].data();
            std::uint64_t off =
                static_cast<std::uint64_t>(submit_k) * c.request_size;
            if (c.op == Op::write)
                fill_from_master(b, c.request_size, rs.master_bytes());
            sluice::async::Completion<std::size_t>& cc = comp[slot];
            auto sr = c.op == Op::read
                          ? ctx.submit_read(
                                sluice::async::ReadOp{rs.fd, b, c.request_size,
                                                      off},
                                cc)
                          : ctx.submit_write(
                                sluice::async::WriteOp{rs.fd, b,
                                                       c.request_size, off},
                                cc);
            if (!sr.has_value())
                bench_semantic("z2: submit rejected (capacity/lifecycle)");
            ++submit_k;
        }
        sluice::async::Completion<std::size_t>& cc = comp[consume_k % D];
        while (!cc.ready()) {
            auto w = ctx.wait_one();
            if (!w.has_value()) bench_semantic("z2: wait_one failed");
        }
        auto res = cc.result();
        if (!res.has_value()) bench_semantic("z2: terminal I/O error");
        if (res.value() != c.request_size)
            bench_semantic("z2: op moved wrong byte count");
        if (c.op == Op::read)
            r.word_sum +=
                word_sum(rs.buf[consume_k % D].data(), c.request_size);
        cc.reset();
        ++consume_k;
        ++r.ops;
        r.bytes += res.value();
    }
    return r;
}

// --- Z3: ApplicationRuntime continuation (E1-L2 shape, workers knob) -------
struct Z3Outcome {
    std::uint64_t word_sum = 0;
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    bool ok = false;
    char err[128] = {0};
};

void z3_task_body(sluice::async::RuntimeTaskContext& ctx, RunState& rs,
                  Z3Outcome& out) {
    using namespace sluice::async;
    const Config& c = rs.cfg;
    const std::size_t D = c.depth;
    std::vector<Completion<std::size_t>> comp(D);
    std::size_t submit_k = 0, await_k = 0;
    while (await_k < rs.ops) {
        while (submit_k < rs.ops && submit_k - await_k < D) {
            std::size_t slot = submit_k % D;
            std::byte* b = rs.buf[slot].data();
            std::uint64_t off =
                static_cast<std::uint64_t>(submit_k) * c.request_size;
            if (c.op == Op::write)
                fill_from_master(b, c.request_size, rs.master_bytes());
            auto sr = c.op == Op::read
                          ? ctx.submit_read(
                                ReadOp{rs.fd, b, c.request_size, off},
                                comp[slot])
                          : ctx.submit_write(
                                WriteOp{rs.fd, b, c.request_size, off},
                                comp[slot]);
            if (!sr.has_value()) {
                std::snprintf(out.err, sizeof(out.err),
                              "z3: submit rejected (code %d)",
                              static_cast<int>(sr.error().code));
                return;
            }
            ++submit_k;
        }
        Completion<std::size_t>& cc = comp[await_k % D];
        auto wr = ctx.await_completion(cc);
        if (!wr.has_value()) {
            std::snprintf(out.err, sizeof(out.err),
                          "z3: await_completion rejected (code %d)",
                          static_cast<int>(wr.error().code));
            return;
        }
        auto res = cc.result();
        if (!res.has_value()) {
            std::snprintf(out.err, sizeof(out.err),
                          "z3: terminal I/O error (code %d, os %d)",
                          static_cast<int>(res.error().code),
                          res.error().os_errno);
            return;
        }
        if (res.value() != c.request_size) {
            std::snprintf(out.err, sizeof(out.err),
                          "z3: op moved %zu bytes (want %zu)", res.value(),
                          c.request_size);
            return;
        }
        if (c.op == Op::read)
            out.word_sum +=
                word_sum(rs.buf[await_k % D].data(), c.request_size);
        cc.reset();
        ++await_k;
        ++out.ops;
        out.bytes += res.value();
    }
    out.ok = true;
}

// ---------------------------------------------------------------------------
// Z1b state-machine self-check (harness-correctness gate; no kernel involved)
// ---------------------------------------------------------------------------

int z1b_self_check() {
    // 1. Matching CQE publishes exactly once.
    {
        Z1bSlot s;
        s.cookie = 7;
        s.state = SlotState::in_flight;
        Z1bWitness w;
        if (!z1b_cqe_terminal(s, 7, 4096, w, /*fatal_on_double=*/false))
            return 1;
        if (s.state != SlotState::terminal || w.terminals != 1) return 1;
        // 2. A second CQE for the same cookie must NOT re-publish (detected).
        if (z1b_cqe_terminal(s, 7, 4096, w, /*fatal_on_double=*/false))
            return 1;
        if (w.terminals != 1) return 1;
    }
    // 3. Wrong-cookie CQE (stale) is dropped, never delivered.
    {
        Z1bSlot s;
        s.cookie = 9;  // next occupant of the slot
        s.state = SlotState::in_flight;
        Z1bWitness w;
        if (z1b_cqe_terminal(s, 2, 4096, w, /*fatal_on_double=*/false))
            return 1;  // stale generation must be dropped
        if (w.stale_dropped != 1) return 1;
        // The live occupant still receives its own terminal.
        if (!z1b_cqe_terminal(s, 9, 4096, w, /*fatal_on_double=*/false))
            return 1;
        if (w.terminals != 1) return 1;
    }
    // 4. CQE naming an empty slot is dropped.
    {
        Z1bSlot s;  // empty
        Z1bWitness w;
        if (z1b_cqe_terminal(s, 3, 4096, w, /*fatal_on_double=*/false))
            return 1;
        if (w.stale_dropped != 1 || w.terminals != 0) return 1;
    }
    std::printf("{\"self_check\":\"pass\"}\n");
    return 0;
}

// ---------------------------------------------------------------------------
// JSON emission (single object, one line per rep inside an array)
// ---------------------------------------------------------------------------

void print_json(const Config& c, const RunState& rs, std::size_t queue_depth,
                const std::vector<RepResult>& warmups,
                const std::vector<RepResult>& measured,
                std::uint64_t setup_ns, std::uint64_t teardown_ns,
                bool write_verified, const char* arm_name) {
    auto rep_json = [](const RepResult& r) {
        std::printf(
            "{\"wall_ns\":%llu,\"user_ns\":%llu,\"sys_ns\":%llu,"
            "\"ops\":%llu,\"bytes\":%llu,\"word_sum\":%llu,\"errors\":%llu,"
            "\"stale_dropped\":%llu,\"outstanding_max\":%llu}",
            static_cast<unsigned long long>(r.wall_ns),
            static_cast<unsigned long long>(r.user_ns),
            static_cast<unsigned long long>(r.sys_ns),
            static_cast<unsigned long long>(r.ops),
            static_cast<unsigned long long>(r.bytes),
            static_cast<unsigned long long>(r.word_sum),
            static_cast<unsigned long long>(r.errors),
            static_cast<unsigned long long>(r.stale_dropped),
            static_cast<unsigned long long>(r.outstanding_max));
    };
    std::printf("{");
    std::printf("\"bench\":\"tax0_z_ladder\",\"arm\":\"%s\"", arm_name);
    std::printf(",\"op\":\"%s\"", c.op == Op::read ? "read" : "write");
    std::printf(",\"request_size\":%zu,\"total_bytes\":%zu", c.request_size,
                c.total_bytes);
    std::printf(",\"depth\":%zu,\"workers\":%u", c.depth, c.workers);
    std::printf(",\"request_capacity\":%zu,\"queue_depth\":%zu", c.depth,
                queue_depth);
    std::printf(",\"ops\":%zu", rs.ops);
    std::printf(",\"warmup_reps\":%zu,\"measured_reps\":%zu", c.warmup,
                c.reps);
    std::printf(",\"setup_ns\":%llu,\"teardown_ns\":%llu",
                static_cast<unsigned long long>(setup_ns),
                static_cast<unsigned long long>(teardown_ns));
    std::printf(",\"write_final_verified\":%s",
                write_verified ? "true" : "false");
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    {
        const auto& m = sluice::async::detail::tax0_ablation_modes();
        std::printf(",\"ablation\":{\"f01_r1\":%s,\"f02_r1\":%s,\"f07_r1\":%s}",
                    m.f01_gate_outstanding_eval ? "true" : "false",
                    m.f02_skip_reap_seq ? "true" : "false",
                    m.f07_skip_extent_reprobes ? "true" : "false");
    }
#endif
    std::printf(",\"warmup\":[");
    for (std::size_t i = 0; i < warmups.size(); ++i) {
        if (i) std::printf(",");
        rep_json(warmups[i]);
    }
    std::printf("],\"reps\":[");
    for (std::size_t i = 0; i < measured.size(); ++i) {
        if (i) std::printf(",");
        rep_json(measured[i]);
    }
    std::printf("]}\n");
}

std::size_t parse_size(const char* s) {
    if (s == nullptr || *s == '\0') return 0;
    std::size_t v = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return 0;
        unsigned d = static_cast<unsigned>(*p - '0');
        if (v > (SIZE_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    return v;
}

int usage_error(const char* argv0, const char* detail) {
    std::fprintf(
        stderr,
        "usage: %s --arm z1|z1b|z1bw|z2|z3 [--op read|write] --file PATH\n"
        "          [--request-size N] [--total-bytes N] [--depth N]\n"
        "          [--workers N (z3 only)] [--reps N] [--warmup N]\n"
        "          [--self-check] [--runner-verify]\n"
        "note: TAX-0B semantic-floor ladder (#250/#259/PR #260). In-flight\n"
        "      depth == request_capacity == ring queue depth (no dispatch\n"
        "      backlog). Requires a real-liburing build; stub fails closed.\n"
        "error: %s\n",
        argv0, detail);
    return 2;
}

#endif  // SLUICE_HAS_LIBURING

}  // namespace

#if !defined(SLUICE_HAS_LIBURING)
// Fail-closed stub: the ladder measures REAL io_uring paths only.
int main() {
    std::fprintf(stderr,
                 "tax0_z_ladder_bench: built without liburing "
                 "(SLUICE_HAS_LIBURING undefined) — no ladder arm can run; "
                 "refusing to fabricate a floor\n");
    return 3;
}
#else

int main(int argc, char** argv) {
    using namespace sluice::async;

    Config cfg;
    const char* arm_name = "z1";
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::string d = std::string(opt) + " requires a value";
                std::exit(usage_error(argv[0], d.c_str()));
            }
            return argv[++i];
        };
        if (a == "--arm") {
            std::string s(next("--arm"));
            if (s == "z1") { cfg.arm = Arm::z1; arm_name = "z1"; }
            else if (s == "z1b") { cfg.arm = Arm::z1b; arm_name = "z1b"; }
            else if (s == "z1bw") { cfg.arm = Arm::z1bw; arm_name = "z1bw"; }
            else if (s == "z2") { cfg.arm = Arm::z2; arm_name = "z2"; }
            else if (s == "z3") { cfg.arm = Arm::z3; arm_name = "z3"; }
            else return usage_error(argv[0], "--arm must be z1|z1b|z1bw|z2|z3");
        } else if (a == "--op") {
            std::string s(next("--op"));
            if (s == "read") cfg.op = Op::read;
            else if (s == "write") cfg.op = Op::write;
            else return usage_error(argv[0], "--op must be read|write");
        } else if (a == "--request-size") {
            cfg.request_size = parse_size(next("--request-size"));
        } else if (a == "--total-bytes") {
            cfg.total_bytes = parse_size(next("--total-bytes"));
        } else if (a == "--depth") {
            cfg.depth = parse_size(next("--depth"));
        } else if (a == "--workers") {
            cfg.workers =
                static_cast<unsigned>(parse_size(next("--workers")));
        } else if (a == "--reps") {
            cfg.reps = parse_size(next("--reps"));
        } else if (a == "--warmup") {
            cfg.warmup = parse_size(next("--warmup"));
        } else if (a == "--file") {
            cfg.file = next("--file");
        } else if (a == "--self-check") {
            cfg.self_check = true;
        } else if (a == "--runner-verify") {
            cfg.runner_verify = true;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // TAX-0D ablation modes (this target only; R0 default).
        } else if (a == "--f01-r1") {
            sluice::async::detail::tax0_ablation_modes().f01_gate_outstanding_eval =
                true;
        } else if (a == "--f02-r1") {
            sluice::async::detail::tax0_ablation_modes().f02_skip_reap_seq = true;
        } else if (a == "--f07-r1") {
            // RE-H0 ATTR-B: cached router-extent treatment (prereg A4).
            sluice::async::detail::tax0_ablation_modes().f07_skip_extent_reprobes =
                true;
#endif
        } else {
            return usage_error(argv[0], "unknown argument");
        }
    }
    std::string cfg_err;
    if (!config_valid(cfg, cfg_err))
        return usage_error(argv[0], cfg_err.c_str());

    if (cfg.self_check) return z1b_self_check();

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

    const std::size_t Q = cfg.depth;  // queue depth == D: no dispatch backlog

    // --- construction / registration (OUTSIDE measured reps) ---------------
    std::uint64_t setup_ns = 0, teardown_ns = 0;
    std::uint64_t s0 = now_ns();

    io_uring raw_ring;
    bool raw_ring_init = false;
    std::unique_ptr<Z1bShared> z1b;
    std::unique_ptr<AsyncIoContext> ctx;
    std::vector<Completion<std::size_t>> comp;
    std::unique_ptr<ApplicationRuntime> rt;

    if (cfg.arm == Arm::z1) {
        if (::io_uring_queue_init(Q, &raw_ring, /*flags=*/0) != 0)
            bench_fatal("io_uring_queue_init", errno);
        raw_ring_init = true;
    } else if (cfg.arm == Arm::z1b || cfg.arm == Arm::z1bw) {
        z1b = std::make_unique<Z1bShared>();
        if (::io_uring_queue_init(Q, &raw_ring, /*flags=*/0) != 0)
            bench_fatal("io_uring_queue_init", errno);
        raw_ring_init = true;
        z1b->ring = &raw_ring;
        z1b->rs = &rs;
        z1b->slots.resize(cfg.depth);
    } else if (cfg.arm == Arm::z2) {
        auto ub = std::make_unique<UringAsyncBackend>(
            UringConfig{cfg.depth, static_cast<unsigned>(Q)});
        if (!ub->available()) {
            std::fprintf(stderr,
                         "tax0_z_ladder_bench: uring backend did not "
                         "initialize a real ring (available()==false)\n");
            return 3;
        }
        ctx = std::make_unique<AsyncIoContext>(std::move(ub));
        comp = std::vector<sluice::async::Completion<std::size_t>>(cfg.depth);
    } else {  // z3
        RuntimeBuilder builder;
        builder.workers(cfg.workers);
        auto ub = std::make_unique<UringAsyncBackend>(
            UringConfig{cfg.depth, static_cast<unsigned>(Q)});
        if (!ub->available()) {
            std::fprintf(stderr,
                         "tax0_z_ladder_bench: uring backend did not "
                         "initialize a real ring (available()==false)\n");
            return 3;
        }
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

    // --- warmup + measured reps --------------------------------------------
    auto run_rep = [&](RepResult& out) {
        if (cfg.arm == Arm::z1) {
            out = z1_run(rs, raw_ring);
        } else if (cfg.arm == Arm::z1b) {
            out = z1b_run(*z1b);
        } else if (cfg.arm == Arm::z1bw) {
            out = z1bw_run(*z1b);
        } else if (cfg.arm == Arm::z2) {
            out = z2_run(rs, *ctx, comp);
        } else {
            CpuTime c0 = cpu_time_now();
            std::uint64_t t0 = now_ns();
            TaskResultSlot<Z3Outcome> slot;
            auto sub = rt->submit([&rs, &slot](RuntimeTaskContext& tctx) {
                Z3Outcome o;
                z3_task_body(tctx, rs, o);
                slot.publish(std::move(o));
            });
            if (!sub.has_value())
                bench_semantic("runtime rejected the task admission");
            Z3Outcome o = slot.wait_and_take();
            out.wall_ns = now_ns() - t0;
            CpuTime c1 = cpu_time_now();
            out.user_ns = c1.user_ns - c0.user_ns;
            out.sys_ns = c1.sys_ns - c0.sys_ns;
            out.ops = o.ops;
            out.bytes = o.bytes;
            out.word_sum = o.word_sum;
            if (!o.ok) {
                std::fprintf(stderr, "tax0_z_ladder_bench: task failed: %s\n",
                             o.err);
                out.errors = 1;
            }
        }
    };

    std::vector<RepResult> warmups;
    for (std::size_t i = 0; i < cfg.warmup; ++i) {
        RepResult w;
        run_rep(w);
        warmups.push_back(w);
    }

    std::vector<RepResult> measured;
    for (std::size_t i = 0; i < cfg.reps; ++i) {
        CpuTime c0 = cpu_time_now();
        std::uint64_t t0 = now_ns();
        RepResult r;
        run_rep(r);
        r.wall_ns = now_ns() - t0;
        CpuTime c1 = cpu_time_now();
        r.user_ns = c1.user_ns - c0.user_ns;
        r.sys_ns = c1.sys_ns - c0.sys_ns;
        measured.push_back(r);
    }

    // --- same-work adjudication (fail-closed) -------------------------------
    bool all_ok = true;
    for (const auto& r : measured) {
        if (r.errors != 0 || r.ops != rs.ops ||
            r.bytes != cfg.total_bytes ||
            (cfg.op == Op::read && r.word_sum != rs.expected_sum))
            all_ok = false;
        if ((cfg.arm == Arm::z1b || cfg.arm == Arm::z1bw) &&
            (r.stale_dropped != 0 || r.outstanding_max > cfg.depth))
            all_ok = false;
    }
    // Warmup reps carry the same fail-closed accounting.
    for (const auto& r : warmups) {
        if (r.errors != 0 || r.ops != rs.ops ||
            r.bytes != cfg.total_bytes ||
            (cfg.op == Op::read && r.word_sum != rs.expected_sum))
            bench_semantic("warmup rep failed same-work accounting");
    }
    bool write_verified = false;
    if (cfg.op == Op::write && !cfg.runner_verify) {
        verify_written_file(cfg, rs.expected_sum);
        write_verified = true;
    }

    // --- teardown (outside measured reps) -----------------------------------
    std::uint64_t t0 = now_ns();
    if (raw_ring_init) ::io_uring_queue_exit(&raw_ring);
    if (rt) {
        rt->request_stop();
        auto drained = rt->drain();
        auto joined = rt->join();
        if (!drained.has_value() || !joined.has_value())
            bench_semantic("runtime drain/join failed");
    }
    teardown_ns = now_ns() - t0;

    if (::close(rs.fd) != 0) bench_fatal("close data file", errno);

    print_json(cfg, rs, Q, warmups, measured, setup_ns, teardown_ns,
               write_verified, arm_name);
    if (!all_ok) bench_semantic("measured rep failed same-work accounting");
    return 0;
}
#endif  // SLUICE_HAS_LIBURING
