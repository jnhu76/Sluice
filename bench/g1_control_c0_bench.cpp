// G1-CONTROL-C0 (#279, governing issue): research-only io_uring
// fixed-FILE mechanism bench + deterministic identity witness. NOT a
// production component — the production UringAsyncBackend has no
// fixed-file capability (audit: research/g1-control-c0/G1-CONTROL-C0-
// AUDIT.md) and must not gain one from this file.
//
// IMPORTANT opcode fact (audit §2/§5): fixed FILE I/O is
// IORING_OP_READ/WRITE + sqe->flags |= IOSQE_FIXED_FILE with sqe->fd =
// SLOT INDEX. IORING_OP_READ_FIXED/WRITE_FIXED is the fixed-BUFFER
// mechanism (RBUF-E0's object) and is deliberately NOT used here.
//
// Modes:
//   --generate   write the deterministic pattern fixture (src) and print
//                its sha256-relevant info; the driver validates the C++
//                generator against its own Python generator.
//   --probe      behavioral host-capability probe: io_uring queue init +
//                feature flags, register_files, fixed READ+WRITE round
//                trip in STRICT read-CQE-before-write-submit order (no
//                IOSQE_IO_LINK; ordering lives in submission structure),
//                unregister, RLIMIT_MEMLOCK, page size.
//   --fileid     FILE-ID-E0 deterministic wrong-target witness: ordinary
//                fd arm (dup2-forced reuse, no sleep/probability) + fixed
//                L0 arm (frozen binding survives process-fd reuse) +
//                replacement honored going forward.
//   --replacement-window  AUDIT §6 boundaries A (prepare->update->submit
//                binds post-update) and D (bound request keeps old target
//                across update).
//   --run        one formal measurement run for a (op, arm, size, depth,
//                file_bytes) cell. Same-work gates are fail-closed
//                (exit 3): exact op accounting, every CQE res ==
//                requested length, zero canceled/error/short terminals,
//                per-slot state-machine validation, no in-flight op at
//                span end, first+last-page content spot check.
//                Registration/lifecycle failures exit 4.
//
// Arms (prereg §4): F0 ordinary fd / F1 fixed file (L0 frozen binding:
// register once before the measured span, unregister once after) / F0-T
// and F1-T under the matched threaded-process condition (K threads spawn
// at setup, each opens the measured file and performs ONE 4 KiB I/O to
// reference the file from a second thread, then parks until teardown).
// The ONLY F0->F1 delta is the file lookup mechanism.
//
// The driver (research/g1-control-c0/scripts/g1_control_c0.py) wraps each
// run under `perf stat -e instructions:u,cycles:u,task-clock`, hashes dst
// post-exit for WRITE runs, and appends raw evidence into the immutable
// session.

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
#include <pthread.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

constexpr std::size_t kBlock = 4096;
constexpr std::uint64_t kSeed = 0xE1E1E1E121212121ull;
constexpr std::size_t kAlign = 4096;
constexpr std::size_t kMaxDepth = 32;
constexpr int kThreadedWorkers = 4;
constexpr std::size_t kThreadIoLen = 4096;

[[noreturn]] void g1_fatal(const char* what, int err) {
    std::fprintf(stderr, "g1_control_c0_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(2);
}

[[noreturn]] void g1_semantic(const char* what) {
    std::fprintf(stderr, "g1_control_c0_bench: semantic failure: %s\n", what);
    std::exit(3);
}

[[noreturn]] void g1_register_fail(const char* what, int err) {
    std::fprintf(stderr,
                 "g1_control_c0_bench: registration lifecycle failure: %s "
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

// Deterministic 4 KiB master tile; file byte at offset o == tile[o % 4096].
// Both READ src and WRITE dst use this pattern (identical across arms).
std::vector<std::byte> make_master_tile() {
    std::vector<std::byte> tile(kBlock);
    auto* w = reinterpret_cast<std::uint64_t*>(tile.data());
    for (std::size_t i = 0; i < kBlock / 8; ++i) w[i] = splitmix64(kSeed + i);
    return tile;
}

// Fill buf[0..len) with the deterministic per-offset pattern.
void fill_pattern(std::byte* buf, std::size_t len, std::uint64_t file_off,
                  const std::vector<std::byte>& tile) {
    for (std::size_t i = 0; i < len; ++i)
        buf[i] = tile[(file_off + i) % kBlock];
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string bool_s(bool b) { return b ? "true" : "false"; }

// ---- CLI --------------------------------------------------------------

struct Config {
    std::string mode;
    std::string op = "READ";
    std::string arm = "F0";
    std::size_t size = 4096;
    std::size_t depth = 1;
    std::uint64_t file_bytes = 1ull << 30;
    std::string src, dst, label;
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* w) -> std::string {
            if (i + 1 >= argc) g1_fatal(w, EINVAL);
            return argv[++i];
        };
        if (a == "--mode") {
            c.mode = next("--mode");
        } else if (a == "--op") {
            c.op = next("--op");
        } else if (a == "--arm") {
            c.arm = next("--arm");
        } else if (a == "--size") {
            c.size = std::strtoull(next("--size").c_str(), nullptr, 10);
        } else if (a == "--depth") {
            c.depth = std::strtoull(next("--depth").c_str(), nullptr, 10);
        } else if (a == "--file-bytes") {
            c.file_bytes =
                std::strtoull(next("--file-bytes").c_str(), nullptr, 10);
        } else if (a == "--src") {
            c.src = next("--src");
        } else if (a == "--dst") {
            c.dst = next("--dst");
        } else if (a == "--label") {
            c.label = next("--label");
        } else {
            g1_semantic("unknown arg");
        }
    }
    return c;
}

// ---- shared engine ----------------------------------------------------

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

// Threaded-process condition: K worker threads each open the measured file
// and perform ONE 4 KiB ordinary I/O (establishing a second file reference
// in the shared files_struct), then exit. Deterministic: the main thread
// joins each worker before the measured span begins — no sleeps, no parking.
struct WorkerCtx {
    const char* path = nullptr;
    bool is_write = false;
    bool io_ok = false;
    int io_errno = 0;
};

void* worker_entry(void* arg) {
    auto* w = static_cast<WorkerCtx*>(arg);
    int f = ::open(w->path, w->is_write ? O_WRONLY : O_RDONLY);
    if (f < 0) {
        w->io_errno = errno;
        return nullptr;
    }
    std::byte buf[kThreadIoLen];
    ssize_t x = w->is_write ? ::pwrite(f, buf, kThreadIoLen, 0)
                            : ::pread(f, buf, kThreadIoLen, 0);
    if (x == static_cast<ssize_t>(kThreadIoLen)) w->io_ok = true;
    if (x < 0) w->io_errno = errno;
    ::close(f);
    return nullptr;
}

// ---- FILE-ID-E0 witness ----------------------------------------------

// Write a file with a deterministic marker (distinct per id).
void write_marker_file(const char* path, std::uint64_t marker_seed) {
    std::vector<std::byte> tile = make_master_tile();
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) g1_fatal("open(marker)", errno);
    std::vector<std::byte> page(kBlock);
    auto* w = reinterpret_cast<std::uint64_t*>(page.data());
    for (std::size_t i = 0; i < kBlock / 8; ++i)
        w[i] = splitmix64(marker_seed + i);  // distinct from the master tile
    ssize_t x = ::write(fd, page.data(), kBlock);
    if (x != static_cast<ssize_t>(kBlock)) g1_fatal("write(marker)", errno);
    if (::close(fd) != 0) g1_fatal("close(marker)", errno);
}

// Read the first 4 KiB via an io_uring ring (ordinary or fixed slot).
// Returns the marker-seed the content was generated from, or -1 on mismatch.
std::uint64_t read_first_page(io_uring* ring, int fd_or_slot, bool fixed,
                              int* res_out) {
    std::vector<std::byte> buf(kBlock);
    io_uring_sqe* sqe = ::io_uring_get_sqe(ring);
    if (sqe == nullptr) g1_semantic("fileid: no SQE");
    if (fixed) {
        ::io_uring_prep_read(sqe, fd_or_slot, buf.data(), kBlock, 0);
        sqe->flags |= IOSQE_FIXED_FILE;
    } else {
        ::io_uring_prep_read(sqe, fd_or_slot, buf.data(), kBlock, 0);
    }
    ::io_uring_sqe_set_data64(sqe, 0x1d);
    if (::io_uring_submit(ring) != 1) g1_semantic("fileid: submit");
    io_uring_cqe* cqe = nullptr;
    if (::io_uring_wait_cqe(ring, &cqe) < 0)
        g1_fatal("io_uring_wait_cqe(fileid)", EINVAL);
    if (cqe->user_data != 0x1d) g1_semantic("fileid: CQE mismatch");
    const int res = cqe->res;
    *res_out = res;
    ::io_uring_cqe_seen(ring, cqe);
    if (res != static_cast<int>(kBlock)) return -1;
    // Identify which marker: compare against marker A and B pages.
    std::vector<std::byte> pageA(kBlock);
    std::vector<std::byte> pageB(kBlock);
    auto* a = reinterpret_cast<std::uint64_t*>(pageA.data());
    auto* b = reinterpret_cast<std::uint64_t*>(pageB.data());
    for (std::size_t i = 0; i < kBlock / 8; ++i) {
        a[i] = splitmix64(0x4141414101010101ull + i);
        b[i] = splitmix64(0x4242424202020202ull + i);
    }
    if (std::memcmp(buf.data(), pageA.data(), kBlock) == 0) return 0x41;
    if (std::memcmp(buf.data(), pageB.data(), kBlock) == 0) return 0x42;
    return 0;
}

// Determine which marker a plain pread of the first page yields.
std::uint64_t pread_marker(const char* path) {
    std::vector<std::byte> buf(kBlock);
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) g1_fatal("open(marker verify)", errno);
    const ssize_t x = ::pread(fd, buf.data(), kBlock, 0);
    ::close(fd);
    if (x != static_cast<ssize_t>(kBlock)) return 0;
    std::vector<std::byte> pageA(kBlock);
    std::vector<std::byte> pageB(kBlock);
    auto* a = reinterpret_cast<std::uint64_t*>(pageA.data());
    auto* b = reinterpret_cast<std::uint64_t*>(pageB.data());
    for (std::size_t i = 0; i < kBlock / 8; ++i) {
        a[i] = splitmix64(0x4141414101010101ull + i);
        b[i] = splitmix64(0x4242424202020202ull + i);
    }
    if (std::memcmp(buf.data(), pageA.data(), kBlock) == 0) return 0x41;
    if (std::memcmp(buf.data(), pageB.data(), kBlock) == 0) return 0x42;
    return 0;
}

// FILE-ID-E0: deterministic wrong-target witness. A/B markers, dup2-forced
// fd reuse (no sleep, no probabilistic open()==N loop).
int run_fileid(const Config& cfg) {
    if (cfg.src.empty() || cfg.dst.empty())
        g1_semantic("fileid requires --src and --dst dir paths");
    std::string a_path = cfg.src + "/fileid-A.bin";
    std::string b_path = cfg.src + "/fileid-B.bin";
    write_marker_file(a_path.c_str(), 0x4141414101010101ull);
    write_marker_file(b_path.c_str(), 0x4242424202020202ull);

    io_uring ring{};
    const int irc = ::io_uring_queue_init(8, &ring, 0);
    if (irc != 0) g1_register_fail("io_uring_queue_init(fileid)", -irc);

    // ---- ordinary arm: stale fd N reused to B --------------------------
    const int fdN = ::open(a_path.c_str(), O_RDONLY);
    if (fdN < 0) g1_fatal("open(A)", errno);
    const int fdM = ::open(b_path.c_str(), O_RDONLY);
    if (fdM < 0) g1_fatal("open(B)", errno);
    if (::close(fdN) != 0) g1_fatal("close(A)", errno);
    if (::dup2(fdM, fdN) < 0) g1_fatal("dup2(B, N)", errno);  // N -> B
    int ord_res = 0;
    const std::uint64_t ord_marker =
        read_first_page(&ring, fdN, /*fixed=*/false, &ord_res);
    ::close(fdM);

    // ---- fixed L0 arm: register A into slot S, reuse N -> B ------------
    const int fdA2 = ::open(a_path.c_str(), O_RDONLY);
    if (fdA2 < 0) g1_fatal("open(A2)", errno);
    const int fdB2 = ::open(b_path.c_str(), O_RDONLY);
    if (fdB2 < 0) g1_fatal("open(B2)", errno);
    const int slot = 0;
    int reg_errno = 0;
    {
        const int fds[1] = {fdA2};
        const int rrc = ::io_uring_register_files(&ring, fds, 1);
        if (rrc != 0) reg_errno = -rrc;
    }
    int fixed_res = 0;
    std::uint64_t fixed_marker = 0;
    int update_errno = 0;
    int replaced_res = 0;
    std::uint64_t replaced_marker = 0;
    if (reg_errno == 0) {
        // close A's process fd, force N (fdA2) to B2 via dup2
        if (::close(fdA2) != 0) g1_fatal("close(A2)", errno);
        if (::dup2(fdB2, fdA2) < 0) g1_fatal("dup2(B2, N)", errno);
        fixed_marker = read_first_page(&ring, slot, /*fixed=*/true, &fixed_res);
        // now replace slot S <- B2 and re-read: must yield B
        const int fds2[1] = {fdB2};
        const int urc = ::io_uring_register_files_update(&ring, 0, fds2, 1);
        if (urc != 1) update_errno = (urc < 0) ? -urc : urc;
        if (update_errno == 0)
            replaced_marker =
                read_first_page(&ring, slot, /*fixed=*/true, &replaced_res);
    }
    const int urc = ::io_uring_unregister_files(&ring);
    const int unreg_errno = (urc != 0) ? -urc : 0;
    ::close(fdA2);
    ::close(fdB2);
    ::io_uring_queue_exit(&ring);

    const std::uint64_t a_actual = pread_marker(a_path.c_str());
    const std::uint64_t b_actual = pread_marker(b_path.c_str());

    std::printf(
        "{\"mode\":\"fileid\",\"bench\":\"g1_control_c0_bench\","
        "\"a_marker_actual\":\"%c\",\"b_marker_actual\":\"%c\","
        "\"ordinary_fd_N\":%d,\"ordinary_read_res\":%d,"
        "\"ordinary_read_marker\":\"%c\","
        "\"fixed_slot\":%d,\"register_errno\":%d,\"fixed_read_res\":%d,"
        "\"fixed_read_marker\":\"%c\","
        "\"update_errno\":%d,\"replaced_read_res\":%d,"
        "\"replaced_read_marker\":\"%c\",\"unregister_errno\":%d,"
        "\"ordinary_verdict\":\"%s\",\"fixed_verdict\":\"%s\","
        "\"replacement_verdict\":\"%s\"}\n",
        a_actual == 0x41 ? 'A' : '?', b_actual == 0x42 ? 'B' : '?', fdN,
        ord_res, ord_marker == 0x42 ? 'B' : (ord_marker == 0x41 ? 'A' : '?'),
        slot, reg_errno, fixed_res,
        fixed_marker == 0x41 ? 'A' : (fixed_marker == 0x42 ? 'B' : '?'),
        update_errno, replaced_res,
        replaced_marker == 0x42 ? 'B' : (replaced_marker == 0x41 ? 'A' : '?'),
        unreg_errno,
        (ord_marker == 0x42) ? "ORDINARY-FD WRONG-TARGET REPRODUCED" : "INVALID",
        (fixed_marker == 0x41)
            ? "FIXED L0 BINDING PRESERVED TARGET"
            : "FIXED L0 BINDING DID NOT PRESERVE TARGET",
        (replaced_marker == 0x42) ? "REPLACEMENT HONORED GOING FORWARD"
                                  : "INVALID");
    return 0;
}

// AUDIT §6 boundaries A and D (validation->submission->binding window).
int run_replacement_window(const Config& cfg) {
    if (cfg.src.empty() || cfg.dst.empty())
        g1_semantic("replacement-window requires --src and --dst dir paths");
    std::string a_path = cfg.src + "/window-A.bin";
    std::string b_path = cfg.src + "/window-B.bin";
    write_marker_file(a_path.c_str(), 0x4141414101010101ull);
    write_marker_file(b_path.c_str(), 0x4242424202020202ull);

    io_uring ring{};
    const int irc = ::io_uring_queue_init(8, &ring, 0);
    if (irc != 0) g1_register_fail("io_uring_queue_init(window)", -irc);

    const int fdA = ::open(a_path.c_str(), O_RDONLY);
    const int fdB = ::open(b_path.c_str(), O_RDONLY);
    if (fdA < 0 || fdB < 0) g1_fatal("open(window)", errno);
    const int slot = 0;
    const int fdsA[1] = {fdA};
    if (::io_uring_register_files(&ring, fdsA, 1) != 0)
        g1_register_fail("register_files(window A)", EINVAL);

    // ---- boundary A: prepare (no submit) -> update -> submit -----------
    std::vector<std::byte> bufA(kBlock);
    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
    ::io_uring_prep_read(sqe, slot, bufA.data(), kBlock, 0);
    sqe->flags |= IOSQE_FIXED_FILE;
    ::io_uring_sqe_set_data64(sqe, 0xA1);
    // NOT submitted yet. Replace slot S <- B now.
    const int fdsB[1] = {fdB};
    const int ua = ::io_uring_register_files_update(&ring, 0, fdsB, 1);
    const int boundary_a_update_errno = (ua != 1) ? ((ua < 0) ? -ua : ua) : 0;
    // Now submit the prepared SQE; the kernel binds at issue time.
    std::uint64_t boundary_a_marker = 0;
    int boundary_a_res = 0;
    if (::io_uring_submit(&ring) == 1) {
        io_uring_cqe* cqe = nullptr;
        if (::io_uring_wait_cqe(&ring, &cqe) < 0)
            g1_fatal("io_uring_wait_cqe(A)", EINVAL);
        boundary_a_res = cqe->res;
        if (cqe->user_data != 0xA1) g1_semantic("window A: CQE mismatch");
        ::io_uring_cqe_seen(&ring, cqe);
        std::vector<std::byte> pageB(kBlock);
        auto* b = reinterpret_cast<std::uint64_t*>(pageB.data());
        for (std::size_t i = 0; i < kBlock / 8; ++i)
            b[i] = splitmix64(0x4242424202020202ull + i);
        if (boundary_a_res == static_cast<int>(kBlock) &&
            std::memcmp(bufA.data(), pageB.data(), kBlock) == 0)
            boundary_a_marker = 0x42;
    }

    // ---- boundary D: submit while A bound -> reap A -> update -> verify -
    const int fdsA2[1] = {fdA};
    if (::io_uring_register_files_update(&ring, 0, fdsA2, 1) != 1)
        g1_register_fail("register_files_update(window D)", EINVAL);
    std::vector<std::byte> bufD(kBlock);
    std::uint64_t boundary_d_marker = 0;
    int boundary_d_res = 0;
    {
        io_uring_sqe* s = ::io_uring_get_sqe(&ring);
        ::io_uring_prep_read(s, slot, bufD.data(), kBlock, 0);
        s->flags |= IOSQE_FIXED_FILE;
        ::io_uring_sqe_set_data64(s, 0xD1);
        if (::io_uring_submit(&ring) != 1) g1_semantic("window D: submit");
        io_uring_cqe* cqe = nullptr;
        if (::io_uring_wait_cqe(&ring, &cqe) < 0)
            g1_fatal("io_uring_wait_cqe(D)", EINVAL);
        boundary_d_res = cqe->res;
        ::io_uring_cqe_seen(&ring, cqe);
        // request bound A and holds the node; now replace S <- B
        const int ud = ::io_uring_register_files_update(&ring, 0, fdsB, 1);
        if (ud != 1) g1_register_fail("register_files_update(window D2)", EINVAL);
        std::vector<std::byte> pageA(kBlock);
        auto* a = reinterpret_cast<std::uint64_t*>(pageA.data());
        for (std::size_t i = 0; i < kBlock / 8; ++i)
            a[i] = splitmix64(0x4141414101010101ull + i);
        if (boundary_d_res == static_cast<int>(kBlock) &&
            std::memcmp(bufD.data(), pageA.data(), kBlock) == 0)
            boundary_d_marker = 0x41;
    }
    ::io_uring_unregister_files(&ring);
    ::close(fdA);
    ::close(fdB);
    ::io_uring_queue_exit(&ring);

    std::printf(
        "{\"mode\":\"replacement-window\",\"bench\":\"g1_control_c0_bench\","
        "\"boundary_a_update_errno\":%d,\"boundary_a_res\":%d,"
        "\"boundary_a_marker\":\"%c\",\"boundary_a_verdict\":\"%s\","
        "\"boundary_d_res\":%d,\"boundary_d_marker\":\"%c\","
        "\"boundary_d_verdict\":\"%s\"}\n",
        boundary_a_update_errno, boundary_a_res,
        boundary_a_marker == 0x42 ? 'B' : (boundary_a_marker == 0x41 ? 'A' : '?'),
        (boundary_a_marker == 0x42)
            ? "BOUNDARY-A WINDOW CONFIRMED (prepared-before-update SQE binds "
              "post-update)"
            : "INVALID",
        boundary_d_res,
        boundary_d_marker == 0x41 ? 'A' : (boundary_d_marker == 0x42 ? 'B' : '?'),
        (boundary_d_marker == 0x41)
            ? "BOUNDARY-D RETENTION CONFIRMED (bound request kept old target)"
            : "INVALID");
    return 0;
}

// ---- formal run --------------------------------------------------------

int run_one(const Config& cfg) {
    const bool is_read = cfg.op == "READ";
    if (!is_read && cfg.op != "WRITE")
        g1_semantic("op must be READ or WRITE");
    const bool is_fixed = cfg.arm == "F1" || cfg.arm == "F1-T";
    const bool is_threaded = cfg.arm == "F0-T" || cfg.arm == "F1-T";
    if (cfg.arm != "F0" && cfg.arm != "F1" && cfg.arm != "F0-T" &&
        cfg.arm != "F1-T")
        g1_semantic("arm must be F0|F1|F0-T|F1-T");
    if (cfg.size == 0 || cfg.depth == 0 || cfg.depth > kMaxDepth)
        g1_semantic("size > 0, depth in [1,32]");
    if (cfg.file_bytes % cfg.size != 0)
        g1_semantic("file_bytes must be a multiple of size");
    if (cfg.size % kAlign != 0)
        g1_semantic("size must be 4096-aligned");

    struct rusage ru0, ru1;
    if (::getrusage(RUSAGE_SELF, &ru0) != 0) g1_fatal("getrusage(before)", errno);

    const std::uint64_t t_setup0 = now_ns();
    const std::string data_path = is_read ? cfg.src : cfg.dst;
    const int data_fd = ::open(data_path.c_str(),
                               is_read ? O_RDONLY
                                       : (O_WRONLY | O_CREAT | O_TRUNC),
                               is_read ? 0 : 0644);
    if (data_fd < 0) g1_fatal("open(data)", errno);

    void* p = nullptr;
    if (::posix_memalign(&p, kAlign, cfg.size * cfg.depth) != 0)
        g1_fatal("posix_memalign", ENOMEM);
    auto* block = static_cast<std::byte*>(p);
    const std::size_t align_remainder =
        reinterpret_cast<std::uintptr_t>(block) % kAlign;

    io_uring ring{};
    const std::size_t entries = std::max<std::size_t>(8, 2 * cfg.depth);
    {
        const int rc = ::io_uring_queue_init(static_cast<unsigned>(entries),
                                             &ring, /*flags=*/0);
        if (rc != 0) g1_fatal("io_uring_queue_init", -rc);
    }
    const unsigned ring_entries = ring.sq.ring_entries;

    std::uint64_t register_ns = 0;
    if (is_fixed) {
        const std::uint64_t t0 = now_ns();
        const int fds[1] = {data_fd};
        const int rc = ::io_uring_register_files(&ring, fds, 1);
        if (rc != 0) g1_register_fail("io_uring_register_files", -rc);
        register_ns = now_ns() - t0;
    }

    // Threaded condition: spawn K workers that each open the measured file
    // and perform one 4 KiB I/O (second file reference), then park.
    int threads_spawned = 0;
    int threads_io_ok = 0;
    int threads_joined = 0;
    std::vector<pthread_t> ths;
    std::vector<WorkerCtx> wctx;
    if (is_threaded) {
        ths.resize(kThreadedWorkers);
        wctx.resize(kThreadedWorkers);
        for (int i = 0; i < kThreadedWorkers; ++i) {
            wctx[i].path = data_path.c_str();
            wctx[i].is_write = !is_read;
            if (::pthread_create(&ths[i], nullptr, worker_entry, &wctx[i]) !=
                0)
                g1_fatal("pthread_create", EAGAIN);
            ++threads_spawned;
        }
        for (int i = 0; i < kThreadedWorkers; ++i) {
            ::pthread_join(ths[i], nullptr);
            ++threads_joined;
            if (wctx[i].io_ok) ++threads_io_ok;
        }
    }

    const std::uint64_t setup_ns = now_ns() - t_setup0;

    // Master tile built ONCE before the measured span (no per-op allocation
    // inside the span; identical for both arms).
    const std::vector<std::byte> master_tile = make_master_tile();

    // ---- measured span ----
    const std::uint64_t chunks = cfg.file_bytes / cfg.size;
    std::vector<bool> in_flight(cfg.depth, false);
    std::vector<std::uint64_t> next_chunk(cfg.depth, 0);
    for (std::size_t s = 0; s < cfg.depth; ++s) next_chunk[s] = s;
    std::vector<std::uint64_t> pending_chunk(cfg.depth, 0);
    std::vector<std::byte*> slot_buf(cfg.depth, nullptr);
    for (std::size_t s = 0; s < cfg.depth; ++s)
        slot_buf[s] = block + s * cfg.size;

    RunCounters c;
    std::uint64_t ops_done = 0;
    const std::uint64_t target_ops = chunks;
    const std::uint64_t t0 = now_ns();

    auto submit_pass = [&]() -> bool {
        unsigned prepared = 0;
        for (std::size_t s = 0; s < cfg.depth; ++s) {
            if (in_flight[s]) continue;
            const std::uint64_t chunk = next_chunk[s];
            if (chunk >= chunks) continue;
            const std::uint64_t off = chunk * cfg.size;
            io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
            if (sqe == nullptr)
                g1_semantic("SQE unavailable with entries >= 2*depth");
            std::byte* buf = slot_buf[s];
            if (!is_read) fill_pattern(buf, cfg.size, off, master_tile);
            if (is_fixed) {
                // Fixed FILE: IORING_OP_READ/WRITE + IOSQE_FIXED_FILE,
                // sqe->fd = slot index (0). NOT the fixed-BUFFER opcode.
                if (is_read)
                    ::io_uring_prep_read(sqe, 0, buf,
                                         static_cast<unsigned>(cfg.size), off);
                else
                    ::io_uring_prep_write(sqe, 0, buf,
                                          static_cast<unsigned>(cfg.size), off);
                sqe->flags |= IOSQE_FIXED_FILE;
            } else {
                if (is_read)
                    ::io_uring_prep_read(sqe, data_fd, buf,
                                         static_cast<unsigned>(cfg.size), off);
                else
                    ::io_uring_prep_write(sqe, data_fd, buf,
                                          static_cast<unsigned>(cfg.size), off);
            }
            ::io_uring_sqe_set_data64(sqe, s);
            pending_chunk[s] = chunk;
            in_flight[s] = true;
            next_chunk[s] += cfg.depth;
            ++prepared;
        }
        if (prepared == 0) return ops_done >= target_ops;
        const int rc = ::io_uring_submit(&ring);
        if (rc < 0) g1_fatal("io_uring_submit", -rc);
        if (static_cast<unsigned>(rc) != prepared)
            g1_semantic("short io_uring_submit");
        return false;
    };

    // Fill pattern once for WRITE (per op it is deterministic; the fill
    // happens before submit, inside the span — identical for both arms).
    submit_pass();
    while (ops_done < target_ops) {
        io_uring_cqe* cqe = nullptr;
        const int rc = ::io_uring_wait_cqe(&ring, &cqe);
        if (rc < 0) g1_fatal("io_uring_wait_cqe", -rc);
        while (cqe != nullptr) {
            const std::uint64_t s = cqe->user_data;
            if (s >= cfg.depth) g1_semantic("CQE slot out of range");
            if (!in_flight[s]) g1_semantic("CQE does not match slot state");
            ++c.cqe_count;
            if (cqe->res < 0) {
                ++c.errors;
                if (cqe->res == -ECANCELED) ++c.canceled;
                g1_semantic("unexpected error terminal on data-path CQE");
            }
            if (static_cast<std::uint64_t>(cqe->res) != cfg.size) {
                if (is_read)
                    ++c.short_reads;
                else
                    ++c.short_writes;
                g1_semantic("short I/O on data-path CQE (recorded, not retried)");
            }
            if (is_read) {
                c.bytes_read += static_cast<std::uint64_t>(cqe->res);
                ++c.read_ops;
            } else {
                c.bytes_written += static_cast<std::uint64_t>(cqe->res);
                ++c.write_ops;
            }
            ++ops_done;
            in_flight[s] = false;
            ::io_uring_cqe_seen(&ring, cqe);
            cqe = nullptr;
            (void)::io_uring_peek_cqe(&ring, &cqe);
        }
        if (ops_done < target_ops) submit_pass();
    }
    for (std::size_t s = 0; s < cfg.depth; ++s)
        if (in_flight[s]) g1_semantic("in-flight op at span end");
    const std::uint64_t transfer_ns = now_ns() - t0;

    // ---- teardown (outside span) ----
    std::uint64_t unregister_ns = 0;
    if (is_fixed) {
        const std::uint64_t t1 = now_ns();
        const int rc = ::io_uring_unregister_files(&ring);
        if (rc != 0) g1_register_fail("io_uring_unregister_files", -rc);
        unregister_ns = now_ns() - t1;
    }
    ::io_uring_queue_exit(&ring);
    if (::close(data_fd) != 0) g1_fatal("close(data)", errno);
    ::free(block);

    // content spot check (outside span)
    {
        const int vfd = ::open(data_path.c_str(), O_RDONLY);
        if (vfd < 0) g1_fatal("open(spot-check)", errno);
        std::vector<std::byte> page(kBlock);
        const std::vector<std::byte> tile = make_master_tile();
        for (std::uint64_t off : {std::uint64_t{0},
                                  cfg.file_bytes - kBlock}) {
            const ssize_t x = ::pread(vfd, page.data(), kBlock, off);
            if (x != static_cast<ssize_t>(kBlock))
                g1_semantic("spot check: short pread");
            for (std::size_t i = 0; i < kBlock; ++i)
                if (page[i] != tile[(off + i) % kBlock])
                    g1_semantic("spot check: content mismatch");
        }
        ::close(vfd);
    }

    if (::getrusage(RUSAGE_SELF, &ru1) != 0) g1_fatal("getrusage(after)", errno);
    const std::uint64_t utime_us =
        static_cast<std::uint64_t>(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) *
            1000000ull +
        static_cast<std::uint64_t>(ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec);
    const std::uint64_t stime_us =
        static_cast<std::uint64_t>(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) *
            1000000ull +
        static_cast<std::uint64_t>(ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec);

    // ---- same-work gates (fail-closed) ----
    if (c.read_ops != (is_read ? target_ops : 0) ||
        c.write_ops != (is_read ? 0 : target_ops))
        g1_semantic("op count mismatch");
    if (c.cqe_count != target_ops) g1_semantic("CQE count != op count");
    if (c.canceled != 0) g1_semantic("unexpected canceled terminal");
    if (c.errors != 0) g1_semantic("error terminal observed");
    if (is_read && c.bytes_read != cfg.file_bytes)
        g1_semantic("bytes_read != file_bytes");
    if (!is_read && c.bytes_written != cfg.file_bytes)
        g1_semantic("bytes_written != file_bytes");

    const std::uint64_t total_ops = target_ops;
    std::printf(
        "{\"bench\":\"g1_control_c0_bench\",\"mode\":\"run\",\"label\":\"%s\","
        "\"op\":\"%s\",\"arm\":\"%s\",\"size\":%llu,\"depth\":%llu,"
        "\"file_bytes\":%llu,\"chunks\":%llu,\"ring_entries_requested\":%llu,"
        "\"ring_entries\":%u,\"setup_ns\":%llu,\"register_ns\":%llu,"
        "\"unregister_ns\":%llu,\"transfer_ns\":%llu,"
        "\"total_ops\":%llu,\"wall_per_op_ns\":%.3f,"
        "\"bytes_read\":%llu,\"bytes_written\":%llu,"
        "\"read_ops\":%llu,\"write_ops\":%llu,\"cqe_count\":%llu,"
        "\"canceled\":%llu,\"errors\":%llu,"
        "\"short_reads\":%llu,\"short_writes\":%llu,"
        "\"utime_us\":%llu,\"stime_us\":%llu,\"maxrss_kb\":%ld,"
        "\"minflt\":%ld,\"majflt\":%ld,"
        "\"align_remainder\":%zu,\"slot_stride\":%llu,"
        "\"registered_files\":%llu,\"threads_spawned\":%d,"
        "\"threads_io_ok\":%d,\"threads_joined\":%d,"
        "\"data_fd\":%d,\"ok\":true}\n",
        cfg.label.c_str(), cfg.op.c_str(), cfg.arm.c_str(),
        (unsigned long long)cfg.size, (unsigned long long)cfg.depth,
        (unsigned long long)cfg.file_bytes, (unsigned long long)chunks,
        (unsigned long long)entries, ring_entries,
        (unsigned long long)setup_ns, (unsigned long long)register_ns,
        (unsigned long long)unregister_ns, (unsigned long long)transfer_ns,
        (unsigned long long)total_ops,
        static_cast<double>(transfer_ns) /
            static_cast<double>(total_ops ? total_ops : 1),
        (unsigned long long)c.bytes_read, (unsigned long long)c.bytes_written,
        (unsigned long long)c.read_ops, (unsigned long long)c.write_ops,
        (unsigned long long)c.cqe_count, (unsigned long long)c.canceled,
        (unsigned long long)c.errors, (unsigned long long)c.short_reads,
        (unsigned long long)c.short_writes, (unsigned long long)utime_us,
        (unsigned long long)stime_us, ru1.ru_maxrss, ru1.ru_minflt,
        ru1.ru_majflt, align_remainder, (unsigned long long)cfg.size,
        (unsigned long long)(is_fixed ? 1 : 0), threads_spawned,
        threads_io_ok, threads_joined, data_fd);
    return 0;
}

// ---- probe ------------------------------------------------------------

int run_probe(const Config& cfg) {
    if (cfg.src.empty() || cfg.dst.empty())
        g1_semantic("probe requires --src/--dst dir paths");
    std::string r_path = cfg.src + "/probe-src.bin";
    std::string w_path = cfg.src + "/probe-dst.bin";
    const std::vector<std::byte> tile = make_master_tile();
    const int sfd = ::open(r_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sfd < 0) g1_fatal("open(probe src)", errno);
    ssize_t x = ::write(sfd, tile.data(), kBlock);
    if (x != static_cast<ssize_t>(kBlock)) g1_fatal("write(probe src)", errno);
    if (::close(sfd) != 0) g1_fatal("close(probe src)", errno);

    io_uring_params params{};
    int init_errno = 0, reg_errno = 0, unreg_errno = 0;
    int read_res = 0, write_res = 0;
    bool read_ok = false, write_ok = false;
    bool write_submitted_after_read_cqe = false;
    io_uring ring{};
    const int irc = ::io_uring_queue_init_params(8, &ring, &params);
    if (irc != 0) {
        init_errno = -irc;
    } else {
        void* p = nullptr;
        if (::posix_memalign(&p, kAlign, kBlock) != 0)
            g1_fatal("posix_memalign(probe)", ENOMEM);
        auto* buf = static_cast<std::byte*>(p);
        const int rfd = ::open(r_path.c_str(), O_RDONLY);
        const int wfd = ::open(w_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (rfd < 0) g1_fatal("open(probe read)", errno);
        if (wfd < 0) g1_fatal("open(probe write)", errno);
        const int fds[2] = {rfd, wfd};
        const int rrc = ::io_uring_register_files(&ring, fds, 2);
        if (rrc != 0) {
            reg_errno = -rrc;
        } else {
            // STRICT ordering: read completes (CQE reaped, content
            // validated) before the write SQE is prepared. No IO_LINK.
            io_uring_sqe* sq = ::io_uring_get_sqe(&ring);
            ::io_uring_prep_read(sq, 0, buf, kBlock, 0);
            sq->flags |= IOSQE_FIXED_FILE;
            ::io_uring_sqe_set_data64(sq, 1);
            if (::io_uring_submit(&ring) != 1)
                g1_semantic("probe read submit");
            {
                io_uring_cqe* cqe = nullptr;
                const int wrc = ::io_uring_wait_cqe(&ring, &cqe);
                if (wrc < 0) g1_fatal("io_uring_wait_cqe(probe read)", -wrc);
                if (cqe->user_data != 1)
                    g1_semantic("probe: CQE before read completion");
                read_res = cqe->res;
                read_ok = read_res == static_cast<int>(kBlock) &&
                          std::memcmp(buf, tile.data(), kBlock) == 0;
                ::io_uring_cqe_seen(&ring, cqe);
            }
            // write via slot 1 (wfd registered above)
            io_uring_sqe* sw = ::io_uring_get_sqe(&ring);
            ::io_uring_prep_write(sw, 1, buf, kBlock, 0);
            sw->flags |= IOSQE_FIXED_FILE;
            ::io_uring_sqe_set_data64(sw, 2);
            write_submitted_after_read_cqe = true;
            if (::io_uring_submit(&ring) != 1)
                g1_semantic("probe write submit");
            {
                io_uring_cqe* cqe = nullptr;
                const int wrc = ::io_uring_wait_cqe(&ring, &cqe);
                if (wrc < 0)
                    g1_fatal("io_uring_wait_cqe(probe write)", -wrc);
                if (cqe->user_data != 2)
                    g1_semantic("probe: CQE before write completion");
                write_res = cqe->res;
                ::io_uring_cqe_seen(&ring, cqe);
            }
            ::close(rfd);
            ::close(wfd);
            char back[kBlock];
            const int vfd = ::open(w_path.c_str(), O_RDONLY);
            if (vfd < 0) g1_fatal("open(probe verify)", errno);
            const ssize_t g = ::pread(vfd, back, kBlock, 0);
            const bool file_ok =
                g == static_cast<ssize_t>(kBlock) &&
                std::memcmp(back, tile.data(), kBlock) == 0;
            ::close(vfd);
            write_ok = write_res == static_cast<int>(kBlock) && file_ok;
            const int urc = ::io_uring_unregister_files(&ring);
            if (urc != 0) unreg_errno = -urc;
        }
        ::free(p);
        ::io_uring_queue_exit(&ring);
    }
    struct rlimit rl;
    if (::getrlimit(RLIMIT_MEMLOCK, &rl) != 0)
        g1_fatal("getrlimit(RLIMIT_MEMLOCK)", errno);
    const bool ok = init_errno == 0 && reg_errno == 0 && unreg_errno == 0 &&
                    read_ok && write_ok && write_submitted_after_read_cqe;
    std::printf(
        "{\"bench\":\"g1_control_c0_bench\",\"mode\":\"probe\","
        "\"uring_queue_init_errno\":%d,\"register_errno\":%d,"
        "\"unregister_errno\":%d,\"read_fixed_res\":%d,"
        "\"write_fixed_res\":%d,\"read_content_ok\":%s,"
        "\"write_content_ok\":%s,"
        "\"write_submitted_after_read_cqe\":%s,"
        "\"features\":%llu,\"rsrc_tags_feature\":%s,"
        "\"memlock_cur_bytes\":%llu,\"memlock_max_bytes\":%llu,"
        "\"page_size\":%ld,\"capable\":%s}\n",
        init_errno, reg_errno, unreg_errno, read_res, write_res,
        bool_s(read_ok).c_str(), bool_s(write_ok).c_str(),
        bool_s(write_submitted_after_read_cqe).c_str(),
        (unsigned long long)params.features,
        bool_s((params.features & IORING_FEAT_RSRC_TAGS) != 0).c_str(),
        (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max,
        ::sysconf(_SC_PAGESIZE), bool_s(ok).c_str());
    return ok ? 0 : 5;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    if (cfg.mode == "generate") {
        if (cfg.src.empty()) g1_semantic("generate requires --src");
        const std::vector<std::byte> tile = make_master_tile();
        const int fd = ::open(cfg.src.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                              0644);
        if (fd < 0) g1_fatal("open(generate)", errno);
        std::vector<std::byte> page(kBlock);
        std::uint64_t written = 0;
        while (written < cfg.file_bytes) {
            const std::size_t n = static_cast<std::size_t>(
                std::min<std::uint64_t>(kBlock, cfg.file_bytes - written));
            std::memcpy(page.data(), tile.data(), n);
            ssize_t x = ::write(fd, page.data(), n);
            if (x < 0) {
                if (errno == EINTR) continue;
                g1_fatal("write(generate)", errno);
            }
            written += static_cast<std::uint64_t>(x);
        }
        if (::close(fd) != 0) g1_fatal("close(generate)", errno);
        std::printf("generated %s (%llu bytes)\n", cfg.src.c_str(),
                    (unsigned long long)cfg.file_bytes);
        return 0;
    }
    if (cfg.mode == "probe") return run_probe(cfg);
    if (cfg.mode == "fileid") return run_fileid(cfg);
    if (cfg.mode == "replacement-window") return run_replacement_window(cfg);
    if (cfg.mode == "run") return run_one(cfg);
    g1_semantic("--mode required: generate|probe|fileid|replacement-window|run");
}
